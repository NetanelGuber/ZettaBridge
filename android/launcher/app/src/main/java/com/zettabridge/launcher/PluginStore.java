package com.zettabridge.launcher;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Imported plugins on disk. Import copies the APK, extracts arm64 libraries and caches metadata. */
final class PluginStore {
    static final int ARCHIVE_FLAGS = PackageManager.GET_ACTIVITIES | PackageManager.GET_PROVIDERS
            | PackageManager.GET_META_DATA;

    private PluginStore() {}

    static File root(Context c) {
        return new File(c.getFilesDir(), "plugins");
    }

    static List<PluginRecord> list(Context c) {
        List<PluginRecord> out = new ArrayList<>();
        File[] dirs = root(c).listFiles();
        if (dirs == null) return out;
        for (File dir : dirs) {
            PluginRecord r = dir.isDirectory() ? PluginRecord.load(dir) : null;
            if (r != null) out.add(r);
        }
        out.sort((a, b) -> a.label.compareToIgnoreCase(b.label));
        return out;
    }

    static PluginRecord find(Context c, String packageName) {
        if (packageName == null || packageName.contains("/")) return null;
        return PluginRecord.load(new File(root(c), packageName));
    }

    /** Application info of an imported APK, with paths pointing at our copy. */
    static ApplicationInfo applicationInfo(PackageInfo info, PluginRecord r) {
        ApplicationInfo ai = info.applicationInfo;
        ai.sourceDir = r.apk().getPath();
        ai.publicSourceDir = ai.sourceDir;
        ai.nativeLibraryDir = r.libDir().getPath();
        ai.dataDir = r.dataDir().getPath();
        return ai;
    }

    static PluginRecord importApk(Context c, Uri uri) throws IOException {
        File tmp = new File(c.getCacheDir(), "import.apk");
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("cannot open " + uri);
            copy(in, tmp);
        }
        PackageManager pm = c.getPackageManager();
        PackageInfo info = pm.getPackageArchiveInfo(tmp.getPath(), ARCHIVE_FLAGS);
        if (info == null || info.applicationInfo == null) {
            tmp.delete();
            throw new IOException("not an installable APK (split bundles are not supported)");
        }
        if (info.packageName.equals(c.getPackageName()) || info.packageName.contains("/")) {
            tmp.delete();
            throw new IOException("refusing to import " + info.packageName);
        }

        PluginRecord r = new PluginRecord(new File(root(c), info.packageName));
        if (!r.dir.isDirectory() && !r.dir.mkdirs()) throw new IOException("cannot create " + r.dir);
        File apk = r.apk();
        if (apk.exists()) {
            apk.setWritable(true);
            if (!apk.delete()) throw new IOException("cannot replace " + apk);
        }
        if (!tmp.renameTo(apk)) {
            throw new IOException("cannot move the APK into " + r.dir);
        }
        // Android 14+ refuses to load dex code from writable files.
        apk.setReadOnly();

        r.packageName = info.packageName;
        r.versionName = info.versionName;
        ApplicationInfo ai = applicationInfo(info, r);
        r.targetSdk = ai.targetSdkVersion;
        extractLibraries(apk, r.libDir(), r.abis);
        r.dataDir().mkdirs();

        CharSequence label = ai.loadLabel(pm);
        r.label = label != null && label.length() > 0 ? label.toString() : r.packageName;
        saveIcon(ai.loadIcon(pm), r.iconFile());
        try {
            r.launcherActivity = ManifestReader.findLauncherActivity(pm.getResourcesForApplication(ai), r.packageName);
        } catch (PackageManager.NameNotFoundException e) {
            r.launcherActivity = null;
        }
        r.save();
        return r;
    }

    static void delete(PluginRecord r) {
        deleteRecursive(r.dir);
    }

    /** Records every lib/<abi>/ directory and extracts lib/arm64-v8a/*.so (Phase 0 runs arm64 only). */
    private static void extractLibraries(File apk, File libDir, Set<String> abis) throws IOException {
        deleteRecursive(libDir);
        if (!libDir.mkdirs()) throw new IOException("cannot create " + libDir);
        try (ZipFile zip = new ZipFile(apk)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry e = entries.nextElement();
                String[] parts = e.getName().split("/");
                if (parts.length != 3 || !parts[0].equals("lib") || e.isDirectory()) continue;
                abis.add(parts[1]);
                if (!parts[1].equals(PluginRecord.ABI_ARM64) || !parts[2].endsWith(".so")) continue;
                try (InputStream in = zip.getInputStream(e)) {
                    copy(in, new File(libDir, parts[2]));
                }
            }
        }
    }

    private static void saveIcon(Drawable d, File out) {
        if (d == null) return;
        int size = 192;
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        d.setBounds(0, 0, size, size);
        d.draw(canvas);
        try (OutputStream o = new FileOutputStream(out)) {
            bmp.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (IOException ignored) {
            // no icon is fine
        }
    }

    private static void copy(InputStream in, File out) throws IOException {
        try (OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        }
    }

    static void deleteRecursive(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File child : children) deleteRecursive(child);
        }
        f.delete();
    }
}
