package com.zettabridge.step09fixture;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private static final Uri DATA = Uri.parse("content://com.zettabridge.step09fixture.files/fixture.txt");

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        FixtureLog.guest(this, "activity-create");
        try {
            PackageManager pm = getPackageManager();
            ActivityInfo alias = pm.getActivityInfo(new ComponentName(this, LauncherAlias.class), 0);
            ProviderInfo provider = pm.resolveContentProvider("com.zettabridge.step09fixture.files", 0);
            if (!getPackageName().equals(alias.packageName) || provider == null ||
                    !getPackageName().equals(provider.packageName) ||
                    !"preserved".equals(pm.getApplicationInfo(getPackageName(),
                            PackageManager.GET_META_DATA).metaData.getString("step09.application")))
                throw new IllegalStateException("installed package metadata differs");
            try (Cursor cursor = getContentResolver().query(DATA, null, null, null, null)) {
                if (cursor == null || !cursor.moveToFirst() ||
                        !getPackageName().equals(cursor.getString(0)) ||
                        !getPackageName().equals(cursor.getString(1)))
                    throw new IllegalStateException("provider package differs");
            }
            startService(new Intent(this, WorkerService.class));
            Intent alarm = new Intent(this, AlarmReceiver.class).setAction("com.zettabridge.step09fixture.ALARM");
            PendingIntent pending = PendingIntent.getBroadcast(this, 9, alarm,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            AlarmManager manager = getSystemService(AlarmManager.class);
            manager.set(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                    SystemClock.elapsedRealtime() + 5000, pending);
            FixtureLog.event(this, "activity-package-check-pass");
        } catch (Exception failure) {
            FixtureLog.event(this, "activity-fail-" + failure.getClass().getSimpleName());
            throw new IllegalStateException("Step 09 component check failed", failure);
        }
        if (getIntent().getData() != null) FixtureLog.event(this, "deep-link-" + getIntent().getData().getScheme());
        TextView view = new TextView(this);
        view.setText("Step 09 component fixture");
        setContentView(view);
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        FixtureLog.event(this, "activity-new-intent");
        if (intent.getData() != null) FixtureLog.event(this, "deep-link-" + intent.getData().getScheme());
    }
    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        FixtureLog.event(this, "activity-configuration-changed");
    }
    @Override protected void onResume() { super.onResume(); FixtureLog.event(this, "activity-resume"); }
    @Override protected void onPause() { FixtureLog.event(this, "activity-pause"); super.onPause(); }
    @Override protected void onStop() { FixtureLog.event(this, "activity-stop"); super.onStop(); }
    @Override protected void onDestroy() { FixtureLog.event(this, "activity-destroy"); super.onDestroy(); }
}
