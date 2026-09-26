package com.zettabridge.step11fixture;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class ProbeActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("zbstep11probe"); }
    private static native int probe(android.content.res.AssetManager assets, Bitmap bitmap,
                                    android.view.Surface surface);

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        final Bitmap bitmap = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
        final int result = probe(getAssets(), bitmap, holder.getSurface());
        final int firstPixel = bitmap.getPixel(0, 0);
        final String text = String.format("result=0x%08x pixel=0x%08x uid=%d", result,
                firstPixel, android.os.Process.myUid());
        try {
            Files.write(new File(getFilesDir(), "step11-result.txt").toPath(),
                    text.getBytes(StandardCharsets.US_ASCII));
            final Class<?> bridge = Class.forName("com.zettabridge.core.ZBridge");
            final String report = (String)bridge.getMethod("runtimeReport").invoke(null);
            Files.write(new File(getFilesDir(), "step11-runtime-report.txt").toPath(),
                    report.getBytes(StandardCharsets.UTF_8));
        } catch (Exception error) {
            throw new IllegalStateException("Step 11 result write failed", error);
        }
        android.util.Log.i("ZB_STEP11", text);
        if (result != 0x1f || firstPixel != 0xff996633)
            throw new IllegalStateException("Step 11 NDK probe failed: " + text);
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
    @Override public void surfaceDestroyed(SurfaceHolder holder) {}
}
