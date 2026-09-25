package com.zettabridge.step08fixture;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;

import com.zettabridge.step04.Probe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Proves guest JNI can load in an app provider after the injected bootstrap provider. */
public final class StartupProvider extends ContentProvider {
    @Override public boolean onCreate() {
        try {
            System.loadLibrary("zbstep04a");
            boolean passed = Probe.add(5) == 12 && Probe.multiply(4) == 12 && Probe.callbacks == 1;
            Files.write(new File(getContext().getFilesDir(), "step08-provider.txt").toPath(),
                    (passed ? "PASS\n" : "FAIL\n").getBytes(StandardCharsets.US_ASCII));
            if (!passed) throw new IllegalStateException("provider guest JNI result differs");
            return true;
        } catch (Exception failure) {
            throw new IllegalStateException("provider guest JNI failed", failure);
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
