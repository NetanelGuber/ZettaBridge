package com.zettabridge.step08fixture;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.Process;

import com.zettabridge.step04.Probe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Separate process: the injected :worker provider must bootstrap before this callback. */
public final class WorkerService extends Service {
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            System.loadLibrary("zbstep04a");
            boolean passed = Probe.add(6) == 13 && Probe.multiply(5) == 15 && Probe.callbacks == 1;
            Files.write(new File(getFilesDir(), "step08-worker.txt").toPath(),
                    ((passed ? "PASS" : "FAIL") + " uid=" + Process.myUid() + " pid=" + Process.myPid() + "\n")
                            .getBytes(StandardCharsets.US_ASCII));
            if (!passed) throw new IllegalStateException("worker guest JNI result differs");
        } catch (Exception failure) {
            throw new IllegalStateException("Step 08 worker-process probe failed", failure);
        }
        stopSelf(startId);
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
