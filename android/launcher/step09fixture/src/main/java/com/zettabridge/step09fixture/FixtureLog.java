package com.zettabridge.step09fixture;

import android.content.Context;
import android.os.Process;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

final class FixtureLog {
    private FixtureLog() {}

    static void event(Context context, String event) {
        String line = event + " uid=" + Process.myUid() + " pid=" + Process.myPid() +
                " package=" + context.getPackageName() + "\n";
        File file = new File(context.getFilesDir(), "step09-events.txt");
        try (FileOutputStream out = new FileOutputStream(file, true)) {
            out.write(line.getBytes(StandardCharsets.UTF_8));
        } catch (Exception failure) {
            throw new IllegalStateException("cannot record " + event, failure);
        }
    }

    static void guest(Context context, String event) {
        System.loadLibrary("zbstep04a");
        if (com.zettabridge.step04.Probe.add(5) != 12 ||
                com.zettabridge.step04.Probe.multiply(4) != 12)
            throw new IllegalStateException("guest JNI failed in " + event);
        event(context, event);
    }
}
