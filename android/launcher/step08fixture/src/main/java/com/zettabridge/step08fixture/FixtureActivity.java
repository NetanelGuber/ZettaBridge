package com.zettabridge.step08fixture;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class FixtureActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            String provider = new String(Files.readAllBytes(
                    new File(getFilesDir(), "step08-provider.txt").toPath()), StandardCharsets.US_ASCII).trim();
            if (!"PASS".equals(provider)) throw new IllegalStateException("provider did not run first");
            startService(new Intent(this, WorkerService.class));
        } catch (Exception failure) {
            throw new IllegalStateException("Step 08 main-process probe failed", failure);
        }
    }
}
