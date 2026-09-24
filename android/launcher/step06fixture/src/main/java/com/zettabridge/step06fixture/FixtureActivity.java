package com.zettabridge.step06fixture;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Process;
import android.widget.TextView;

import com.zettabridge.step04.Probe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Step 06 identity, permission and app-UID probe; it never requests root. */
public final class FixtureActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String result;
        try {
            System.loadLibrary("zbstep04a");
            int value = Probe.add(5);
            result = (value == 12 && Probe.callbacks == 1 ? "PASS" : "FAIL")
                    + " uid=" + Process.myUid() + " native=" + value
                    + " callbacks=" + Probe.callbacks + " package=" + getPackageName();
        } catch (Throwable failure) {
            result = "FAIL uid=" + Process.myUid() + " error=" + failure;
        }
        try {
            Files.write(new File(getFilesDir(), "step06-result.txt").toPath(),
                    (result + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
        TextView view = new TextView(this);
        view.setText(result);
        setContentView(view);
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, 6);
        }
    }
}
