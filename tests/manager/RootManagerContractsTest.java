package com.zettabridge.manager;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

public final class RootManagerContractsTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Recorder implements RootManager.Provider {
        final List<String> commands = new ArrayList<>();
        byte[] input;
        RootManager.Result answer = new RootManager.Result(RootManager.State.GRANTED, "Success");
        @Override public RootManager.Result run(String command, java.io.InputStream stream,
                                               long length, RootManager.Cancellation cancel, long timeout) {
            commands.add(command);
            try { input = stream == null ? null : stream.readAllBytes(); }
            catch (Exception e) { throw new AssertionError(e); }
            check(timeout == RootManager.TIMEOUT_MS, "bounded deadline");
            return answer;
        }
    }

    private static void checkPackageNames() {
        for (String valid : new String[] {"com.example.app", "a.b", "_a.b2"}) {
            check(RootManager.validPackage(valid), "valid package " + valid);
        }
        for (String bad : new String[] {"a", "a..b", ".a", "a.b;id", "a.b\nwhoami",
                "a.b/c", "a.b'", "1a.b", "a.b ", "a.b$"}) {
            check(!RootManager.validPackage(bad), "reject package " + bad);
        }
    }

    private static void checkConsentAndCommands(File dir) throws Exception {
        Recorder provider = new Recorder();
        RootManager manager = new RootManager(dir, provider);
        RootManager.Cancellation cancel = new RootManager.Cancellation();
        check(manager.checkGrant(cancel).state == RootManager.State.DENIED,
                "a nonzero su UID must be denied");
        provider.answer = new RootManager.Result(RootManager.State.GRANTED, "0");
        check(manager.checkGrant(cancel).succeeded(), "UID zero grants root");
        provider.commands.clear();
        check(manager.uninstall("com.example.app", false, (action, consequence) -> false, cancel)
                .state == RootManager.State.CANCELLED, "unconfirmed uninstall denied");
        check(provider.commands.isEmpty(), "unconfirmed action did not contact su");
        provider.answer = new RootManager.Result(RootManager.State.GRANTED, "Success");
        check(manager.uninstall("com.example.app", false, (action, consequence) -> {
            check(consequence.contains("delete its app data"), "data-loss consequence shown");
            return true;
        }, cancel).succeeded(), "explicit uninstall");
        check(provider.commands.get(0).contains("pm uninstall"), "fixed uninstall command");
        check(!provider.commands.get(0).contains("com.example.app"), "package is not shell source");
        check("com.example.app\n".equals(new String(provider.input, StandardCharsets.US_ASCII)),
                "package passed on stdin");
        check(manager.uninstall("com.example.app", true, (a, b) -> true, cancel).succeeded(),
                "explicit keep-data removal");
        check(provider.commands.get(1).contains("uninstall -k"), "keep-data action separate");
        check(manager.uninstall("a.b;id", false, (a, b) -> true, cancel).state
                == RootManager.State.FAILED, "malformed package rejected");
        check(provider.commands.size() == 2, "malformed package never reached su");
        cancel.cancel();
        check(manager.uninstall("a.b", false, (a, b) -> true, cancel).state
                == RootManager.State.CANCELLED, "cancellation fails closed");
    }

    private static void checkPathReplacement(File dir) throws Exception {
        Recorder provider = new Recorder();
        RootManager manager = new RootManager(dir, provider);
        File outside = new File(dir.getParentFile(), "outside.apk");
        try (FileOutputStream out = new FileOutputStream(outside)) { out.write(1); }
        File replaced = new File(dir, "selected.apk");
        Files.createSymbolicLink(replaced.toPath(), outside.toPath());
        RootManager.StagedApk apk = new RootManager.StagedApk(replaced, 1, "00");
        check(manager.install(apk, false, (a, b) -> true, new RootManager.Cancellation()).state
                == RootManager.State.FAILED, "symlink replacement denied");
        check(provider.commands.isEmpty(), "path replacement never reached su");
        Files.delete(replaced.toPath());
        outside.delete();
    }

    private static void checkStagingCleanup(File root) throws Exception {
        File dir = new File(root, "cleanup");
        check(dir.mkdir(), "cleanup directory");
        File stale = new File(dir, "selected-old.apk");
        Files.writeString(stale.toPath(), "old");
        File unrelated = new File(dir, "keep.txt");
        Files.writeString(unrelated.toPath(), "keep");
        File outside = new File(root, "outside.txt");
        Files.writeString(outside.toPath(), "outside");
        Files.createSymbolicLink(new File(dir, "selected-link.apk").toPath(), outside.toPath());
        new RootManager(dir, new Recorder()).cleanupStaleStaging();
        check(!stale.exists(), "abandoned APK removed");
        check(!Files.exists(new File(dir, "selected-link.apk").toPath(),
                java.nio.file.LinkOption.NOFOLLOW_LINKS), "staging link removed");
        check(outside.exists() && unrelated.exists(), "unrelated files remain");
        unrelated.delete();
        outside.delete();
        dir.delete();
    }

    private static void checkProcessFailures(File dir) throws Exception {
        File fakeSu = new File(dir, "fake-su.sh");
        Files.writeString(fakeSu.toPath(), "#!/bin/sh\ncase \"$2\" in\n"
                + "  slow) exec sleep 5 ;;\n  fail) echo denied; exit 7 ;;\n"
                + "  '/system/bin/id -u') echo denied; exit 7 ;;\n"
                + "  *) echo 0 ;;\nesac\n");
        check(fakeSu.setExecutable(true), "fake su executable");
        RootManager.KernelSuProvider provider = new RootManager.KernelSuProvider(fakeSu.getAbsolutePath());
        check(provider.run("fail", null, 0, new RootManager.Cancellation(), 1000).state
                == RootManager.State.FAILED, "command failure reported");
        check(provider.run("/system/bin/id -u", null, 0, new RootManager.Cancellation(), 1000).state
                == RootManager.State.DENIED, "provider grant denial reported");
        check(provider.run("slow", null, 0, new RootManager.Cancellation(), 100).state
                == RootManager.State.TIMED_OUT, "timeout kills command");
        RootManager.Cancellation cancelled = new RootManager.Cancellation();
        Thread canceller = new Thread(() -> {
            try { Thread.sleep(80); } catch (InterruptedException ignored) { }
            cancelled.cancel();
        });
        canceller.start();
        check(provider.run("slow", new ByteArrayInputStream(new byte[0]), 0, cancelled, 2000).state
                == RootManager.State.CANCELLED, "cancellation kills command");
        canceller.join();
    }

    public static void main(String[] args) throws Exception {
        File dir = Files.createTempDirectory("zb-root-contracts-").toFile();
        try {
            checkPackageNames();
            checkConsentAndCommands(dir);
            checkPathReplacement(dir);
            checkStagingCleanup(dir);
            checkProcessFailures(dir);
        } finally {
            for (File child : dir.listFiles()) child.delete();
            dir.delete();
        }
        System.out.println("RootManagerContractsTest PASS");
    }
}
