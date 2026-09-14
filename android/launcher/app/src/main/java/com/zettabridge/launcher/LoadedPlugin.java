package com.zettabridge.launcher;

import android.app.Application;
import android.app.Instrumentation;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.util.Log;

import dalvik.system.DexClassLoader;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/** A plugin loaded into the :guest process: its class loader, resources and Application. */
final class LoadedPlugin {
    private static final String TAG = "zb-launcher";

    final PluginRecord record;
    final String packageName;
    final String label;
    final ApplicationInfo appInfo;
    final Resources resources;
    final ClassLoader classLoader;
    /** Plugin activities by fully qualified class name. */
    final Map<String, ActivityInfo> activities = new HashMap<>();
    Application application;

    private LoadedPlugin(PluginRecord record, ApplicationInfo appInfo, Resources resources, ClassLoader classLoader) {
        this.record = record;
        this.packageName = record.packageName;
        this.label = record.label;
        this.appInfo = appInfo;
        this.resources = resources;
        this.classLoader = classLoader;
    }

    Bitmap icon() {
        return record.icon();
    }

    static LoadedPlugin load(Application host, PluginRecord record) throws Exception {
        PackageManager pm = host.getPackageManager();
        PackageInfo info = pm.getPackageArchiveInfo(record.apk().getPath(), PluginStore.ARCHIVE_FLAGS);
        if (info == null) throw new IllegalStateException("cannot parse " + record.apk());
        ApplicationInfo ai = PluginStore.applicationInfo(info, record);
        ai.uid = android.os.Process.myUid();
        // Public API; works for an APK that is not installed once sourceDir points at it.
        Resources res = pm.getResourcesForApplication(ai);

        File codeCache = new File(host.getCodeCacheDir(), "plugins/" + record.packageName);
        codeCache.mkdirs();
        record.dataDir().mkdirs();
        // Parent is the boot class loader, so the plugin never sees launcher classes.
        ClassLoader cl = new DexClassLoader(record.apk().getPath(), codeCache.getPath(), record.libDir().getPath(),
                android.content.Context.class.getClassLoader());

        LoadedPlugin p = new LoadedPlugin(record, ai, res, cl);
        if (info.activities != null) {
            for (ActivityInfo a : info.activities) {
                a.applicationInfo = ai;
                p.activities.put(a.name, a);
            }
        }

        String appClass = ai.className != null ? ai.className : Application.class.getName();
        PluginContext appContext = new PluginContext(host.getBaseContext(), p);
        p.application = (Application) Instrumentation.newApplication(cl.loadClass(appClass), appContext);
        Log.i(TAG, "plugin " + p.packageName + ": application " + appClass + " attached");
        p.application.onCreate();
        return p;
    }
}
