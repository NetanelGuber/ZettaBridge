package com.zettabridge.bootstrap;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Process;
import android.util.Log;

import com.zettabridge.core.ZBridge;

import java.io.File;

/** Starts the installed bridge before normal Activity/Service/Receiver callbacks. */
public class BootstrapProvider extends ContentProvider {
    private static final String TAG = "ZettaBridge";

    @Override public boolean onCreate() {
        try {
            // Keep one report per process launch so a crash remains inspectable after a restart.
            File reports = getContext().getExternalFilesDir("zb-reports");
            if (reports == null) reports = new File(getContext().getFilesDir(), "zb-reports");
            if (!reports.isDirectory() && !reports.mkdirs())
                throw new IllegalStateException("cannot create runtime report directory " + reports);
            File report = new File(reports, "zb-runtime-" + Process.myPid() + ".txt");
            if (!ZBridge.setReportFile(report.getCanonicalPath()))
                throw new IllegalStateException("cannot persist runtime report to " + report);
            Log.i(TAG, "runtime report: " + report);
            RuntimeInstaller.install(getContext());
            File files = getContext().getFilesDir();
            ZBridge.activateInstalled(files.getCanonicalPath(),
                    new File(getContext().getApplicationInfo().nativeLibraryDir).getCanonicalPath(),
                    getContext().getApplicationInfo().targetSdkVersion,
                    getContext().getClassLoader());
            return true;
        } catch (Throwable failure) {
            throw new IllegalStateException("ZettaBridge bootstrap failed", failure);
        }
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) { throw new UnsupportedOperationException(); }
    @Override public String getType(Uri uri) { throw new UnsupportedOperationException(); }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
    @Override public int update(Uri uri, ContentValues values, String selection,
            String[] selectionArgs) { throw new UnsupportedOperationException(); }
}
