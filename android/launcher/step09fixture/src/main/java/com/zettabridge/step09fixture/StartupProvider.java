package com.zettabridge.step09fixture;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;

public final class StartupProvider extends ContentProvider {
    @Override public boolean onCreate() {
        FixtureLog.guest(getContext(), "startup-provider");
        return true;
    }
    @Override public Cursor query(Uri uri, String[] p, String s, String[] a, String order) {
        throw new UnsupportedOperationException();
    }
    @Override public String getType(Uri uri) { throw new UnsupportedOperationException(); }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String s, String[] a) {
        throw new UnsupportedOperationException();
    }
}
