package com.zettabridge.step09fixture;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

public final class WorkerService extends Service {
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        FixtureLog.guest(this, "worker-service");
        stopSelf(startId);
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
