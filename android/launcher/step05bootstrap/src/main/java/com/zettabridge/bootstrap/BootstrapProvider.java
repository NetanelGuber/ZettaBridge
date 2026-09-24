package com.zettabridge.bootstrap;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;

import com.zettabridge.core.ZBridge;

import java.io.File;

/** Starts the installed bridge before normal Activity/Service/Receiver callbacks. */
public final class BootstrapProvider extends ContentProvider {
    @Override public boolean onCreate() {
        try {
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
