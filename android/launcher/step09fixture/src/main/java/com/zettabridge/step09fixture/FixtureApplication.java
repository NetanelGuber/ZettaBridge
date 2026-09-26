package com.zettabridge.step09fixture;

import android.app.Application;
import android.content.Context;
import java.io.File;

public final class FixtureApplication extends Application {
    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        // Android constructs Application before providers; no native load is attempted here.
        FixtureLog.event(this, "application-attach");
    }

    @Override public void onCreate() {
        super.onCreate();
        if (!new File(getFilesDir(), "zb/.bundle-version").isFile())
            throw new IllegalStateException("bootstrap missing before Application.onCreate");
        FixtureLog.guest(this, "application-create");
    }
}
