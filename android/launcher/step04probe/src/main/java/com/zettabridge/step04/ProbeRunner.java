package com.zettabridge.step04;

import android.content.Context;
import android.os.Process;

import com.zettabridge.core.ZBridge;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

final class ProbeRunner {
    private ProbeRunner() {}

    static String run(Context context, String label) {
        String result;
        try {
            RuntimeInstaller.install(context);
            File report = new File(context.getFilesDir(), "step04-runtime-" + label + ".txt");
            if (!ZBridge.setReportFile(report.getAbsolutePath()))
                throw new IOException("cannot persist runtime report");
            ZBridge.activateInstalled(context.getFilesDir().getCanonicalPath(),
                    new File(context.getApplicationInfo().nativeLibraryDir).getCanonicalPath(),
                    context.getApplicationInfo().targetSdkVersion, Probe.class.getClassLoader());
            System.loadLibrary("zbstep04a");
            if (Probe.add(5) != 12 || Probe.multiply(6) != 18 || Probe.callbacks != 1)
                throw new AssertionError("first ARM32/JNI result or callback is wrong");
            System.loadLibrary("zbstep04b");
            if (Probe.subtract(20) != 16 || Probe.callbacks != 2)
                throw new AssertionError("second ARM32/JNI result or callback is wrong");
            // ART and the bridge must keep the first library live after loading another one.
            System.loadLibrary("zbstep04a");
            if (Probe.add(8) != 15 || Probe.callbacks != 2)
                throw new AssertionError("repeat load changed the process state");
            result = "PASS uid=" + Process.myUid() + " package=" + context.getPackageName()
                    + " process=" + android.app.Application.getProcessName()
                    + " native=12,18,16,15 callbacks=" + Probe.callbacks;
        } catch (Throwable failure) {
            String detail;
            try { detail = ZBridge.lastLoadError(); } catch (Throwable ignored) { detail = null; }
            result = "FAIL uid=" + Process.myUid() + " package=" + context.getPackageName()
                    + " process=" + android.app.Application.getProcessName() + " error=" + failure
                    + " bridge=" + detail;
        }
        try {
            Files.write(new File(context.getFilesDir(), "step04-result-" + label + ".txt").toPath(),
                    (result + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) {
            result += " result-write-error=" + error;
        }
        return result;
    }
}
