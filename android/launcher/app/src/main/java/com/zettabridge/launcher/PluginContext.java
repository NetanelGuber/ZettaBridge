package com.zettabridge.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.view.LayoutInflater;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;

/**
 * Base context of plugin activities and the plugin Application. Code, resources and storage
 * belong to the plugin; identity (package name, uid, system services) stays the launcher's,
 * because system services check the caller's package against its uid.
 */
final class PluginContext extends ContextWrapper {
    private final LoadedPlugin plugin;
    private Resources.Theme theme;
    private LayoutInflater inflater;

    PluginContext(Context base, LoadedPlugin plugin) {
        super(base);
        this.plugin = plugin;
    }

    @Override
    public Resources getResources() {
        return plugin.resources;
    }

    @Override
    public AssetManager getAssets() {
        return plugin.resources.getAssets();
    }

    @Override
    public ClassLoader getClassLoader() {
        return plugin.classLoader;
    }

    @Override
    public ApplicationInfo getApplicationInfo() {
        return plugin.appInfo;
    }

    @Override
    public String getPackageCodePath() {
        return plugin.appInfo.sourceDir;
    }

    @Override
    public String getPackageResourcePath() {
        return plugin.appInfo.sourceDir;
    }

    @Override
    public Context getApplicationContext() {
        return plugin.application != null ? plugin.application : super.getApplicationContext();
    }

    // Theme and inflater are only used when this context is not wrapped by an Activity
    // (the plugin Application); activities have their own ContextThemeWrapper state.
    @Override
    public Resources.Theme getTheme() {
        if (theme == null) {
            theme = plugin.resources.newTheme();
            int id = plugin.appInfo.theme != 0 ? plugin.appInfo.theme : android.R.style.Theme_DeviceDefault_Light_DarkActionBar;
            theme.applyStyle(id, true);
        }
        return theme;
    }

    @Override
    public void setTheme(int resid) {
        getTheme().applyStyle(resid, true);
    }

    @Override
    public Object getSystemService(String name) {
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            if (inflater == null) inflater = LayoutInflater.from(getBaseContext()).cloneInContext(this);
            return inflater;
        }
        return super.getSystemService(name);
    }

    // Storage: each plugin gets filesDir/plugins/<package>/data and its own external subdirectory.

    private File dir(String name) {
        File f = new File(plugin.record.dataDir(), name);
        f.mkdirs();
        return f;
    }

    private static File external(File base, String sub) {
        if (base == null) return null;
        File f = new File(base, sub);
        f.mkdirs();
        return f;
    }

    @Override
    public File getDataDir() {
        return plugin.record.dataDir();
    }

    @Override
    public File getFilesDir() {
        return dir("files");
    }

    @Override
    public File getCacheDir() {
        return dir("cache");
    }

    @Override
    public File getNoBackupFilesDir() {
        return dir("no_backup");
    }

    @Override
    public File getDir(String name, int mode) {
        return dir("app_" + name);
    }

    @Override
    public File getFileStreamPath(String name) {
        return new File(getFilesDir(), name);
    }

    @Override
    public FileInputStream openFileInput(String name) throws FileNotFoundException {
        return new FileInputStream(getFileStreamPath(name));
    }

    @Override
    public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
        return new FileOutputStream(getFileStreamPath(name), (mode & MODE_APPEND) != 0);
    }

    @Override
    public boolean deleteFile(String name) {
        return getFileStreamPath(name).delete();
    }

    @Override
    public String[] fileList() {
        String[] names = getFilesDir().list();
        return names != null ? names : new String[0];
    }

    @Override
    public File getDatabasePath(String name) {
        if (name.startsWith(File.separator)) return new File(name);
        return new File(dir("databases"), name);
    }

    @Override
    public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory) {
        return SQLiteDatabase.openOrCreateDatabase(getDatabasePath(name), factory);
    }

    @Override
    public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory,
                                              DatabaseErrorHandler errorHandler) {
        return SQLiteDatabase.openOrCreateDatabase(getDatabasePath(name).getPath(), factory, errorHandler);
    }

    @Override
    public boolean deleteDatabase(String name) {
        return SQLiteDatabase.deleteDatabase(getDatabasePath(name));
    }

    @Override
    public String[] databaseList() {
        String[] names = dir("databases").list();
        return names != null ? names : new String[0];
    }

    @Override
    public android.content.SharedPreferences getSharedPreferences(String name, int mode) {
        return super.getSharedPreferences(plugin.packageName + "__" + name, mode);
    }

    @Override
    public boolean deleteSharedPreferences(String name) {
        return super.deleteSharedPreferences(plugin.packageName + "__" + name);
    }

    @Override
    public File getExternalFilesDir(String type) {
        String sub = "plugins/" + plugin.packageName + (type != null ? "/" + type : "");
        return external(super.getExternalFilesDir(null), sub);
    }

    @Override
    public File[] getExternalFilesDirs(String type) {
        return new File[] {getExternalFilesDir(type)};
    }

    @Override
    public File getExternalCacheDir() {
        return external(super.getExternalCacheDir(), "plugins/" + plugin.packageName);
    }

    @Override
    public File[] getExternalCacheDirs() {
        return new File[] {getExternalCacheDir()};
    }

    @Override
    public File getObbDir() {
        return external(super.getObbDir(), plugin.packageName);
    }

    @Override
    public File[] getObbDirs() {
        return new File[] {getObbDir()};
    }
}
