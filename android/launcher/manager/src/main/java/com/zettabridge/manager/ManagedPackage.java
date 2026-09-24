package com.zettabridge.manager;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Process;
import android.os.UserManager;
import android.util.AtomicFile;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.zip.ZipFile;

/** Unprivileged validation and local recovery record for one converted base APK, user 0. */
final class ManagedPackage {
    private static final int FLAGS = PackageManager.GET_SIGNING_CERTIFICATES;
    private final Context context;
    private final File records;

    ManagedPackage(Context context) {
        this.context = context;
        this.records = new File(context.getFilesDir(), "install-records");
    }

    static final class Review {
        final String packageName;
        final String signer;
        final long version;
        final String apkHash;
        final boolean update;
        final int previousUid;
        final long previousVersion;
        final String report;

        Review(String packageName, String signer, long version, String apkHash,
               boolean update, int previousUid, long previousVersion, String report) {
            this.packageName = packageName;
            this.signer = signer;
            this.version = version;
            this.apkHash = apkHash;
            this.update = update;
            this.previousUid = previousUid;
            this.previousVersion = previousVersion;
            this.report = report;
        }

        String summary() {
            return (update ? "Update" : "Install") + " " + packageName + " for user 0\n"
                    + "Version: " + version + (update ? " (previous " + previousVersion + ")" : "")
                    + "\nSigner SHA-256: " + signer + "\nAPK SHA-256: " + apkHash
                    + "\nAndroid will assign the app UID and handle permission prompts."
                    + (update ? " Existing app data is left to PackageManager." : "");
        }
    }

    Review review(RootManager.StagedApk staged, String report) throws Exception {
        if (staged == null || report == null || report.length() > 1_000_000) {
            throw new Exception("select one converted APK and its transformation.json");
        }
        JSONObject metadata = new JSONObject(report);
        if (metadata.getInt("schema") != 1) throw new Exception("unsupported conversion report schema");
        JSONArray files = metadata.getJSONArray("files");
        if (files.length() != 1 || !JSONObject.NULL.equals(files.getJSONObject(0).opt("split"))) {
            throw new Exception("this manager release accepts one base APK; split sets require a later installer");
        }
        String packageName = metadata.getString("package");
        if (!RootManager.validPackage(packageName) || packageName.equals(context.getPackageName())) {
            throw new Exception("invalid or manager package name");
        }
        // The report is untrusted; bind it to the actual staged bytes and parsed package.
        String reportedHash = files.getJSONObject(0).getString("output_sha256");
        if (!reportedHash.equalsIgnoreCase(staged.sha256)
                || !hash(staged.file).equalsIgnoreCase(staged.sha256)) {
            throw new Exception("APK hash differs from conversion report or staged bytes");
        }
        PackageManager pm = context.getPackageManager();
        PackageInfo archive = pm.getPackageArchiveInfo(staged.file.getAbsolutePath(), FLAGS);
        if (archive == null || archive.applicationInfo == null || archive.signingInfo == null) {
            throw new Exception("PackageManager cannot read APK identity or signer");
        }
        String signer = signer(archive);
        long version = archive.getLongVersionCode();
        if (!packageName.equals(archive.packageName)
                || !String.valueOf(version).equals(metadata.getString("version_code"))
                || !signer.equalsIgnoreCase(metadata.getString("output_signer_sha256"))) {
            throw new Exception("APK package, version or signer differs from conversion report");
        }
        if (!packageName.equals(archive.applicationInfo.processName)) {
            throw new Exception("custom default process is not supported by this installer release");
        }
        File anchor = new File(records, "personal-signer.sha256");
        if (anchor.exists() && !signer.equals(new String(
                Files.readAllBytes(anchor.toPath()), StandardCharsets.US_ASCII).trim())) {
            throw new Exception("output signer differs from the manager's personal key fingerprint");
        }
        if (archive.sharedUserId != null) throw new Exception("shared UID is unsupported");
        try (ZipFile zip = new ZipFile(staged.file)) {
            boolean bridge = zip.getEntry("lib/arm64-v8a/libzbridge.so") != null;
            boolean bootstrap = zip.getEntry("assets/zb-files.txt") != null;
            if (!bridge || !bootstrap) throw new Exception("converted bridge/bootstrap missing");
            java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                String name = entries.nextElement().getName();
                if (name.startsWith("lib/") && name.endsWith(".so")
                        && !name.startsWith("lib/arm64-v8a/")) {
                    throw new Exception("non-ARM64 library in host namespace");
                }
            }
        }
        PackageInfo installed = installed(packageName);
        if (installed == null) {
            return new Review(packageName, signer, version, staged.sha256, false, -1, -1, report);
        }
        if (!signer.equals(signer(installed))) {
            throw new Exception("Original signer conflict: keep the installed app. Cancel this install; no removal or data change was made. Renaming is not available in this release.");
        }
        if (installed.applicationInfo == null || installed.applicationInfo.uid < 10000
                || installed.applicationInfo.uid == Process.myUid()) {
            throw new Exception("installed UID is unsafe or belongs to the manager");
        }
        if (version < installed.getLongVersionCode()) {
            throw new Exception("version downgrade is blocked; keep a compatible newer output for recovery");
        }
        return new Review(packageName, signer, version, staged.sha256, true,
                installed.applicationInfo.uid, installed.getLongVersionCode(), report);
    }

    RootManager.Result install(RootManager root, RootManager.StagedApk staged, Review approved,
                               RootManager.Cancellation cancel) {
        boolean rootInvoked = false;
        try {
            Review now = review(staged, approved.report);
            if (!same(now, approved)) throw new Exception("package state changed since confirmation");
            if (!((UserManager) context.getSystemService(Context.USER_SERVICE)).isSystemUser()) {
                throw new Exception("manager must run as Android user 0");
            }
            RootManager.Result users = root.checkPrimaryUserOnly(cancel);
            if (!users.succeeded()) return users;
            record(now, "requested", null);
            rootInvoked = true;
            RootManager.Result result = root.install(staged, now.update, (a, b) -> true, cancel);
            if (!result.succeeded()) {
                record(now, "failed-or-unknown", result.detail);
                return result;
            }
            PackageInfo observed = installed(now.packageName);
            if (observed == null || observed.applicationInfo == null
                    || !now.signer.equals(signer(observed))
                    || observed.getLongVersionCode() != now.version
                    || observed.applicationInfo.uid < 10000
                    || (now.update && observed.applicationInfo.uid != now.previousUid)
                    || !now.packageName.equals(observed.applicationInfo.processName)) {
                record(now, "verify-failed", "Inspect PackageManager state before retrying");
                return new RootManager.Result(RootManager.State.FAILED,
                        "PackageManager returned success but installed identity/process/UID verification failed; inspect state before retrying");
            }
            record(now, "installed", "uid=" + observed.applicationInfo.uid
                    + " process=" + observed.applicationInfo.processName
                    + " nativeLibraryDir=" + observed.applicationInfo.nativeLibraryDir);
            File anchor = new File(records, "personal-signer.sha256");
            if (!anchor.exists()) {
                AtomicFile atomic = new AtomicFile(anchor);
                FileOutputStream out = atomic.startWrite();
                try {
                    out.write((now.signer + "\n").getBytes(StandardCharsets.US_ASCII));
                    atomic.finishWrite(out);
                } catch (Exception e) {
                    atomic.failWrite(out);
                    throw e;
                }
            }
            return new RootManager.Result(RootManager.State.GRANTED,
                    "Verified " + now.packageName + " version=" + now.version
                    + " signer=" + now.signer + " uid=" + observed.applicationInfo.uid
                    + " process=" + observed.applicationInfo.processName
                    + " nativeLibraryDir=" + observed.applicationInfo.nativeLibraryDir);
        } catch (Exception e) {
            return new RootManager.Result(RootManager.State.FAILED,
                    e.getMessage() + (rootInvoked ? "; inspect PackageManager state before retrying" : ""));
        }
    }

    RootManager.Result remove(RootManager root, String packageName, boolean keepData,
                              RootManager.Confirmation confirmation, RootManager.Cancellation cancel) {
        boolean rootInvoked = false;
        try {
            JSONObject current = readRecord(packageName);
            if (current == null || !"installed".equals(current.optString("state"))) {
                throw new Exception("no verified converted install record for this package");
            }
            PackageInfo observed = installed(packageName);
            if (observed == null || !current.getString("signer").equals(signer(observed))) {
                throw new Exception("installed package missing or signer changed; removal blocked");
            }
            if (!((UserManager) context.getSystemService(Context.USER_SERVICE)).isSystemUser()) {
                throw new Exception("manager must run as Android user 0");
            }
            RootManager.Result users = root.checkPrimaryUserOnly(cancel);
            if (!users.succeeded()) return users;
            rootInvoked = true;
            RootManager.Result result = root.uninstall(packageName, keepData, confirmation, cancel);
            if (!result.succeeded()) return result;
            if (installed(packageName) != null) {
                return new RootManager.Result(RootManager.State.FAILED,
                        "PackageManager reported removal but package remains visible; inspect user-0 state");
            }
            current.put("state", keepData ? "removed-keep-data" : "removed-delete-data");
            writeRecord(packageName, current);
            return new RootManager.Result(RootManager.State.GRANTED,
                    "Removed " + packageName + " for user 0; "
                    + (keepData ? "PackageManager was asked to retain data" : "PackageManager deleted app data"));
        } catch (Exception e) {
            return new RootManager.Result(RootManager.State.FAILED,
                    e.getMessage() + (rootInvoked ? "; inspect PackageManager state before retrying" : ""));
        }
    }

    /** Owner-selected metadata export; contains no signing key or package data. */
    String recoveryBackup(String packageName) throws Exception {
        JSONObject current = readRecord(packageName);
        if (current == null) throw new Exception("no converted install record to export");
        JSONObject backup = new JSONObject();
        backup.put("schema", 1).put("kind", "zettabridge-install-recovery")
                .put("package", packageName).put("record", current);
        JSONObject attempt = readRecord(packageName + ".attempt");
        if (attempt != null) backup.put("last_attempt", attempt);
        File anchor = new File(records, "personal-signer.sha256");
        if (anchor.isFile()) backup.put("personal_signer_sha256", new String(
                Files.readAllBytes(anchor.toPath()), StandardCharsets.US_ASCII).trim());
        return backup.toString(2) + "\n";
    }

    private static boolean same(Review a, Review b) {
        return a.packageName.equals(b.packageName) && a.signer.equals(b.signer)
                && a.version == b.version && a.apkHash.equals(b.apkHash)
                && a.update == b.update && a.previousUid == b.previousUid
                && a.previousVersion == b.previousVersion;
    }

    private PackageInfo installed(String packageName) throws Exception {
        try { return context.getPackageManager().getPackageInfo(packageName, FLAGS); }
        catch (PackageManager.NameNotFoundException ignored) { return null; }
    }

    private static String signer(PackageInfo info) throws Exception {
        if (info.signingInfo == null || info.signingInfo.hasMultipleSigners()) {
            throw new Exception("missing or multiple signers are unsupported");
        }
        Signature[] signatures = info.signingInfo.getApkContentsSigners();
        if (signatures == null || signatures.length != 1) throw new Exception("one signer required");
        return hex(MessageDigest.getInstance("SHA-256").digest(signatures[0].toByteArray()));
    }

    private static String hash(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = Files.newInputStream(file.toPath())) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) >= 0) digest.update(buf, 0, n);
        }
        return hex(digest.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }

    private File recordFile(String packageName) throws Exception {
        if (!RootManager.validPackage(packageName)) throw new Exception("invalid package name");
        if (!records.isDirectory() && !records.mkdirs()) throw new Exception("cannot create private install records");
        return new File(records, packageName + ".json");
    }

    private JSONObject readRecord(String packageName) throws Exception {
        File file = recordFile(packageName);
        if (!file.exists()) return null;
        return new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
    }

    private void record(Review review, String state, String detail) throws Exception {
        JSONObject previous = readRecord(review.packageName);
        JSONObject value = new JSONObject();
        value.put("schema", 1).put("package", review.packageName)
                .put("signer", review.signer).put("version", review.version)
                .put("output_sha256", review.apkHash).put("state", state)
                .put("timestamp_ms", System.currentTimeMillis())
                .put("prior_version", review.previousVersion)
                .put("prior_uid", review.previousUid)
                .put("detail", detail == null ? JSONObject.NULL : detail)
                .put("conversion", new JSONObject(review.report));
        if ("installed".equals(state) && previous != null) {
            previous.remove("previous_record");
            value.put("previous_record", previous);
        }
        if ("installed".equals(state)) writeRecord(review.packageName, value);
        else writeRecord(review.packageName + ".attempt", value);
    }

    private void writeRecord(String packageName, JSONObject value) throws Exception {
        AtomicFile atomic = new AtomicFile(recordFile(packageName));
        FileOutputStream out = atomic.startWrite();
        try {
            out.write(value.toString(2).getBytes(StandardCharsets.UTF_8));
            atomic.finishWrite(out);
        } catch (Exception e) {
            atomic.failWrite(out);
            throw e;
        }
    }
}
