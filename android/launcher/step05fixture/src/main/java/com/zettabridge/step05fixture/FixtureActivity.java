package com.zettabridge.step05fixture;

import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.widget.TextView;

import com.zettabridge.step04.Probe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Synthetic ARM32 JNI app; no runtime or launcher code is in the source APK. */
public final class FixtureActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String result;
        try {
            System.loadLibrary("zbstep04a");
            int value = Probe.add(5);
            result = (value == 12 && Probe.callbacks == 1 ? "PASS" : "FAIL") +
                    " uid=" + Process.myUid() + " native=" + value +
                    " callbacks=" + Probe.callbacks + " package=" + getPackageName();
        } catch (Throwable failure) {
            result = "FAIL uid=" + Process.myUid() + " error=" + failure;
        }
        try {
            Files.write(new File(getFilesDir(), "step05-result.txt").toPath(),
                    (result + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
        TextView view = new TextView(this);
        view.setText(result);
        setContentView(view);
    }
}
