package com.zettabridge.launcher;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import java.util.List;

/**
 * Switches the :guest process from one plugin to another.
 *
 * The guest runtime is process-lifetime: guest threads cannot be torn down, so one :guest process
 * serves exactly one plugin. A game that exits frees the process by itself; one that keeps
 * running (Flutter does) holds it, and the next game cannot start. This activity runs in the
 * launcher's own process, ends the guest process and starts the requested plugin in a fresh one.
 */
public class PluginSwitchActivity extends Activity {
    private static final String TAG = "zb-launcher";
    private static final String EXTRA_PACKAGE = "zb.package";
    private static final int WAIT_STEPS = 20;
    private static final long WAIT_STEP_MS = 50;

    static Intent intent(Context context, String packageName) {
        return new Intent()
                .setClass(context, PluginSwitchActivity.class)
                .putExtra(EXTRA_PACKAGE, packageName)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final String packageName = getIntent().getStringExtra(EXTRA_PACKAGE);
        Toast.makeText(this, "Closing the previous game...", Toast.LENGTH_SHORT).show();
        killGuestProcess();
        waitForGuestToGo(0, packageName);
    }

    private void killGuestProcess() {
        try {
            ActivityManager manager = getSystemService(ActivityManager.class);
            List<ActivityManager.RunningAppProcessInfo> running =
                    manager != null ? manager.getRunningAppProcesses() : null;
            if (running == null) return;
            for (ActivityManager.RunningAppProcessInfo info : running) {
                if (info.processName != null && info.processName.endsWith(ZbApplication.GUEST_SUFFIX)) {
                    Log.i(TAG, "ending the guest process " + info.pid + " to switch plugins");
                    android.os.Process.killProcess(info.pid);
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot end the guest process: " + e);
        }
    }

    /** The new process must not start before the old one is gone, or it inherits the old plugin. */
    private void waitForGuestToGo(int step, String packageName) {
        if (step < WAIT_STEPS && guestIsRunning()) {
            new Handler(Looper.getMainLooper()).postDelayed(() -> waitForGuestToGo(step + 1, packageName),
                    WAIT_STEP_MS);
            return;
        }
        startActivity(GuestLaunchActivity.intent(this, packageName));
        finish();
    }

    private boolean guestIsRunning() {
        try {
            ActivityManager manager = getSystemService(ActivityManager.class);
            List<ActivityManager.RunningAppProcessInfo> running =
                    manager != null ? manager.getRunningAppProcesses() : null;
            if (running == null) return false;
            for (ActivityManager.RunningAppProcessInfo info : running) {
                if (info.processName != null && info.processName.endsWith(ZbApplication.GUEST_SUFFIX)) return true;
            }
            return false;
        } catch (RuntimeException e) {
            return false;
        }
    }
}
