package com.zettabridge.manager;

import android.content.ContentResolver;
import android.net.Uri;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Manager-only privileged operations. Never pass this object to a guest process or class loader. */
final class RootManager {
    static final long MAX_APK_BYTES = 2L * 1024 * 1024 * 1024;
    static final long TIMEOUT_MS = 120_000;
    private static final String ID = "/system/bin/id -u";
    private static final String INSTALL_NEW = "/system/bin/pm install -R --user 0 -S %d";
    private static final String INSTALL_REPLACE = "/system/bin/pm install -r --user 0 -S %d";
    // The script is constant. The validated package is sent on stdin as data, never shell source.
    private static final String REMOVE = "IFS= read -r pkg; exec /system/bin/pm uninstall --user 0 \"$pkg\"";
    private static final String REMOVE_KEEP_DATA =
            "IFS= read -r pkg; exec /system/bin/pm uninstall -k --user 0 \"$pkg\"";

    interface Provider {
        Result run(String fixedCommand, InputStream input, long length, Cancellation cancel, long timeoutMs);
    }

    interface Confirmation {
        boolean approve(String action, String consequence);
    }

    static final class Cancellation {
        private final AtomicBoolean cancelled = new AtomicBoolean();
        void cancel() { cancelled.set(true); }
        boolean isCancelled() { return cancelled.get(); }
    }

    enum State { GRANTED, DENIED, CANCELLED, TIMED_OUT, FAILED }

    static final class Result {
        final State state;
        final String detail;
        Result(State state, String detail) {
            this.state = state;
            this.detail = detail;
        }
        boolean succeeded() { return state == State.GRANTED; }
    }

    static final class StagedApk {
        final File file;
        final long size;
        final String sha256;
        StagedApk(File file, long size, String sha256) {
            this.file = file;
            this.size = size;
            this.sha256 = sha256;
        }
    }

    private final Provider provider;
    private final File privateDir;

    RootManager(File privateDir, Provider provider) {
        this.privateDir = privateDir;
        this.provider = provider;
    }

    static RootManager kernelSu(File privateDir) {
        return new RootManager(privateDir, new KernelSuProvider("su"));
    }

    /** Remove only abandoned manager-owned staging files after a process restart. */
    void cleanupStaleStaging() throws IOException {
        if (Files.isSymbolicLink(privateDir.toPath())) throw new IOException("staging directory is a link");
        if (!privateDir.exists()) return;
        if (!privateDir.isDirectory()) throw new IOException("staging path is not a directory");
        File[] files = privateDir.listFiles();
        if (files == null) throw new IOException("cannot list staging directory");
        for (File file : files) {
            if (file.getName().startsWith("selected-") && file.getName().endsWith(".apk")
                    && (Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)
                        || Files.isSymbolicLink(file.toPath()))
                    && !file.delete()) {
                throw new IOException("cannot remove abandoned staged APK");
            }
        }
    }

    Result checkGrant(Cancellation cancel) {
        Result result = provider.run(ID, null, 0, cancel, TIMEOUT_MS);
        if (result.succeeded() && !result.detail.trim().equals("0")) {
            return new Result(State.DENIED, "su returned a non-root UID: " + result.detail);
        }
        return result;
    }

    /** Copy a selected content URI while unprivileged. No source path is passed to su. */
    StagedApk stage(ContentResolver resolver, Uri selected, Cancellation cancel) throws IOException {
        if (selected == null || !"content".equals(selected.getScheme())) {
            throw new IOException("select an APK through the document picker");
        }
        if (cancel.isCancelled()) throw new IOException("staging cancelled");
        if (Files.isSymbolicLink(privateDir.toPath())) throw new IOException("staging directory is a link");
        if (!privateDir.isDirectory() && !privateDir.mkdirs()) throw new IOException("cannot create private staging");
        File staged = Files.createTempFile(privateDir.toPath(), "selected-", ".apk").toFile();
        boolean complete = false;
        try (InputStream in = resolver.openInputStream(selected);
             OutputStream out = Files.newOutputStream(staged.toPath(),
                     StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
            if (in == null) throw new IOException("document cannot be opened");
            MessageDigest digest = sha256();
            byte[] buffer = new byte[65536];
            long size = 0;
            int count;
            while ((count = in.read(buffer)) != -1) {
                if (cancel.isCancelled()) throw new IOException("staging cancelled");
                if (count > MAX_APK_BYTES - size) throw new IOException("selected APK exceeds size limit");
                out.write(buffer, 0, count);
                digest.update(buffer, 0, count);
                size += count;
            }
            out.flush();
            if (size == 0) throw new IOException("selected APK is empty");
            complete = true;
            return new StagedApk(staged, size, hex(digest.digest()));
        } finally {
            if (!complete) staged.delete();
        }
    }

    Result install(StagedApk apk, boolean replace, Confirmation confirmation, Cancellation cancel) {
        String action = replace ? "Replace installed package" : "Install new package";
        String consequence = installConsequence(apk, replace);
        if (confirmation == null || !confirmation.approve(action, consequence)) {
            return new Result(State.CANCELLED, "install not confirmed");
        }
        if (cancel.isCancelled()) return new Result(State.CANCELLED, "cancelled");
        try (InputStream input = openVerified(apk)) {
            String command = String.format(java.util.Locale.ROOT,
                    replace ? INSTALL_REPLACE : INSTALL_NEW, apk.size);
            return provider.run(command, input, apk.size, cancel, TIMEOUT_MS);
        } catch (IOException e) {
            return new Result(State.FAILED, "staged APK rejected: " + e.getMessage());
        }
    }

    Result uninstall(String packageName, boolean keepData, Confirmation confirmation, Cancellation cancel) {
        if (!validPackage(packageName)) return new Result(State.FAILED, "invalid package name");
        String consequence = uninstallConsequence(packageName, keepData);
        if (confirmation == null || !confirmation.approve("Uninstall package", consequence)) {
            return new Result(State.CANCELLED, "uninstall not confirmed");
        }
        if (cancel.isCancelled()) return new Result(State.CANCELLED, "cancelled");
        byte[] data = (packageName + "\n").getBytes(StandardCharsets.US_ASCII);
        return provider.run(keepData ? REMOVE_KEEP_DATA : REMOVE,
                new java.io.ByteArrayInputStream(data), data.length, cancel, TIMEOUT_MS);
    }

    static String installConsequence(StagedApk apk, boolean replace) {
        return replace
                ? "PackageManager may replace an existing app. A different signer will be rejected; existing data must not be cleared. SHA-256: " + apk.sha256
                : "PackageManager will install this signed APK for primary user 0 as a normal app with its own UID. Existing packages will not be replaced. SHA-256: " + apk.sha256;
    }

    static String uninstallConsequence(String packageName, boolean keepData) {
        return keepData
                ? "Remove " + packageName + " for primary user 0; its data may be retained by PackageManager."
                : "Remove " + packageName + " for primary user 0 and delete its app data. This cannot be undone here.";
    }

    private InputStream openVerified(StagedApk apk) throws IOException {
        try {
            File canonicalDir = privateDir.getCanonicalFile();
            if (Files.isSymbolicLink(privateDir.toPath())
                    || !apk.file.getParentFile().getCanonicalFile().equals(canonicalDir)
                    || Files.isSymbolicLink(apk.file.toPath())
                    || !Files.isRegularFile(apk.file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("path left private staging or became a link");
            }
            FileDescriptor fd = Os.open(apk.file.getAbsolutePath(),
                    OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW | OsConstants.O_CLOEXEC, 0);
            FileInputStream in = new FileInputStream(fd);
            try {
                StructStat stat = Os.fstat(fd);
                if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_size != apk.size
                        || apk.size <= 0 || apk.size > MAX_APK_BYTES) {
                    throw new IOException("staged file size or type changed");
                }
                MessageDigest digest = sha256();
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) != -1) digest.update(buffer, 0, count);
                if (!hex(digest.digest()).equals(apk.sha256)) throw new IOException("staged hash changed");
                in.getChannel().position(0);
                return in;
            } catch (Exception e) {
                in.close();
                throw e;
            }
        } catch (Exception e) {
            throw e instanceof IOException ? (IOException) e : new IOException(e);
        }
    }

    static boolean validPackage(String name) {
        if (name == null || name.length() > 255 || !name.contains(".")) return false;
        String[] parts = name.split("\\.", -1);
        for (String part : parts) {
            if (!part.matches("[A-Za-z_][A-Za-z0-9_]*")) return false;
        }
        return parts.length >= 2;
    }

    private static MessageDigest sha256() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }

    /** KernelSU su adapter. Only constants above and a decimal byte count reach the shell. */
    static final class KernelSuProvider implements Provider {
        private final String executable;
        KernelSuProvider(String executable) { this.executable = executable; }

        @Override public Result run(String fixedCommand, InputStream input, long length,
                                    Cancellation cancel, long timeoutMs) {
            if (cancel.isCancelled()) return new Result(State.CANCELLED, "cancelled before root request");
            Process process;
            try {
                process = new ProcessBuilder(executable, "-c", fixedCommand)
                        .redirectErrorStream(true).start();
            } catch (IOException e) {
                return new Result(State.FAILED, "KernelSU/su unavailable: " + e.getMessage());
            }
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            AtomicBoolean ioFailed = new AtomicBoolean();
            Thread reader = new Thread(() -> {
                try (InputStream stream = process.getInputStream()) {
                    byte[] bytes = new byte[1024];
                    int count;
                    while ((count = stream.read(bytes)) != -1) {
                        synchronized (output) {
                            if (output.size() < 4096) output.write(bytes, 0,
                                    Math.min(count, 4096 - output.size()));
                        }
                    }
                } catch (IOException e) { ioFailed.set(true); }
            }, "zb-su-output");
            Thread writer = new Thread(() -> {
                try (OutputStream stream = process.getOutputStream()) {
                    if (input != null) {
                        byte[] bytes = new byte[65536];
                        long remaining = length;
                        while (remaining > 0 && !cancel.isCancelled()) {
                            int count = input.read(bytes, 0, (int) Math.min(bytes.length, remaining));
                            if (count < 0) throw new IOException("staged input ended early");
                            stream.write(bytes, 0, count);
                            remaining -= count;
                        }
                        if (remaining != 0) throw new IOException("input cancelled");
                    }
                } catch (IOException e) { ioFailed.set(true); }
            }, "zb-su-input");
            reader.setDaemon(true);
            writer.setDaemon(true);
            reader.start();
            writer.start();
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
            try {
                while (true) {
                    if (cancel.isCancelled()) return stop(process, State.CANCELLED, "root operation cancelled");
                    if (System.nanoTime() >= deadline) return stop(process, State.TIMED_OUT, "root operation timed out");
                    if (process.waitFor(50, TimeUnit.MILLISECONDS)) break;
                }
                writer.join(1000);
                reader.join(1000);
                if (writer.isAlive() || reader.isAlive()) {
                    return new Result(State.FAILED, "root command streams did not finish");
                }
                String detail;
                synchronized (output) {
                    detail = new String(output.toByteArray(), StandardCharsets.UTF_8).trim();
                }
                if (ioFailed.get()) return new Result(State.FAILED, "root command stream failed: " + detail);
                if (process.exitValue() != 0) return new Result(
                        fixedCommand.equals(ID) ? State.DENIED : State.FAILED,
                        "root command exited " + process.exitValue() + ": " + detail);
                if (fixedCommand.equals(ID)) return new Result(State.GRANTED, detail);
                if (!detail.startsWith("Success")) return new Result(State.FAILED,
                        "PackageManager did not report success: " + detail);
                return new Result(State.GRANTED, detail);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return stop(process, State.CANCELLED, "root operation interrupted");
            }
        }

        private static Result stop(Process process, State state, String detail) {
            process.destroyForcibly();
            return new Result(state, detail
                    + "; if PackageManager already started, verify package state before retrying");
        }
    }
}
