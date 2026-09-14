package com.zettabridge.launcher;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

/**
 * Entry point of a plugin (library tap or pinned shortcut), running in :guest. Loads the plugin
 * and starts its launcher activity through a stub; if the plugin is already on screen, finishing
 * simply brings its task to the front.
 */
public class GuestLaunchActivity extends Activity {
    private static final String TAG = "zb-launcher";
    static final String EXTRA_PACKAGE = "com.zettabridge.launcher.PACKAGE";

    static Intent intent(Context context, String packageName) {
        return new Intent(Intent.ACTION_MAIN)
                .setClass(context, GuestLaunchActivity.class)
                .putExtra(EXTRA_PACKAGE, packageName)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String packageName = getIntent().getStringExtra(EXTRA_PACKAGE);
        try {
            launch(packageName);
        } catch (Throwable t) {
            Log.e(TAG, "cannot launch " + packageName, t);
            Toast.makeText(this, "Cannot launch " + packageName + ": " + t, Toast.LENGTH_LONG).show();
        }
        finish();
    }

    private void launch(String packageName) throws Exception {
        GuestRuntime runtime = GuestRuntime.get();
        if (!runtime.isInstalled()) throw new IllegalStateException("guest runtime is not installed");
        if (runtime.isRunning(packageName)) return;

        PluginRecord record = PluginStore.find(this, packageName);
        if (record == null) throw new IllegalStateException("not imported (remove this shortcut)");
        if (!record.isLaunchable()) throw new IllegalStateException(record.status());

        runtime.load(packageName);
        Intent target = new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(packageName, record.launcherActivity);
        Intent routed = runtime.route(target);
        if (routed == target) throw new IllegalStateException(record.launcherActivity + " is not a plugin activity");
        startActivity(routed);
    }
}
