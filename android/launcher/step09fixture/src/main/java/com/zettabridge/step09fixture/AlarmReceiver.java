package com.zettabridge.step09fixture;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;

public final class AlarmReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        FixtureLog.guest(context, "alarm-receiver");
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("step09", "Step 09 fixture",
                NotificationManager.IMPORTANCE_DEFAULT));
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            Notification notification = new Notification.Builder(context, "step09")
                    .setSmallIcon(android.R.drawable.stat_notify_more).setContentTitle("Step 09 fixture")
                    .setContentText("Android-delivered alarm").build();
            manager.notify(9, notification);
            FixtureLog.event(context, "notification-posted");
        } else {
            FixtureLog.event(context, "notification-permission-denied");
        }
    }
}
