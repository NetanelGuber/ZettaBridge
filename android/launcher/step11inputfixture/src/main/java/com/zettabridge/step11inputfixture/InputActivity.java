package com.zettabridge.step11inputfixture;

import android.app.NativeActivity;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputQueue;
import android.view.MotionEvent;

public final class InputActivity extends NativeActivity {
    static { System.loadLibrary("zbinputtest"); }
    private static native void onQueue(InputQueue queue);
    private static native void onMotion(MotionEvent event);

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final long now = SystemClock.uptimeMillis();
        final MotionEvent event = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN,
                12.5f, 7.5f, 0);
        onMotion(event);
        event.recycle();
    }

    @Override public void onInputQueueCreated(InputQueue queue) {
        onQueue(queue);
    }
}
