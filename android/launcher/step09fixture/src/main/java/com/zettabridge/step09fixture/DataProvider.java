package com.zettabridge.step09fixture;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public final class DataProvider extends ContentProvider {
    @Override public boolean onCreate() {
        FixtureLog.guest(getContext(), "data-provider");
        return true;
    }
    @Override public Cursor query(Uri uri, String[] p, String s, String[] a, String order) {
        FixtureLog.guest(getContext(), "data-query");
        MatrixCursor result = new MatrixCursor(new String[] {"package", "resource_package"});
        result.addRow(new Object[] {getContext().getPackageName(),
                getContext().getResources().getResourcePackageName(R.string.step09_marker)});
        return result;
    }
    @Override public String getType(Uri uri) { return "text/plain"; }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws java.io.FileNotFoundException {
        if (!"r".equals(mode) || !"/fixture.txt".equals(uri.getPath()))
            throw new java.io.FileNotFoundException(uri.toString());
        File file = new File(getContext().getFilesDir(), "step09-shared.txt");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write("step09-file-provider\n".getBytes(StandardCharsets.US_ASCII));
        } catch (java.io.IOException failure) {
            throw new java.io.FileNotFoundException(failure.toString());
        }
        FixtureLog.event(getContext(), "data-open-file");
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String s, String[] a) {
        throw new UnsupportedOperationException();
    }
}
