package com.zettabridge.launcher;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;

/** One imported app: filesDir/plugins/<package>/{base.apk, lib/, data/, icon.png, meta.properties}. */
final class PluginRecord {
    static final String ABI_ARM64 = "arm64-v8a";

    final File dir;
    String packageName;
    String label;
    String launcherActivity;
    String versionName;
    int targetSdk;
    /** Every ABI directory found under lib/ in the APK. */
    final Set<String> abis = new TreeSet<>();

    private Bitmap icon;
    private boolean iconLoaded;

    PluginRecord(File dir) {
        this.dir = dir;
    }

    File apk() {
        return new File(dir, "base.apk");
    }

    File libDir() {
        return new File(dir, "lib");
    }

    File dataDir() {
        return new File(dir, "data");
    }

    File iconFile() {
        return new File(dir, "icon.png");
    }

    boolean hasArm64() {
        return abis.contains(ABI_ARM64);
    }

    boolean has32Bit() {
        return abis.contains("armeabi") || abis.contains("armeabi-v7a");
    }

    boolean isLaunchable() {
        return launcherActivity != null && (abis.isEmpty() || hasArm64());
    }

    String status() {
        if (launcherActivity == null) return "no launcher activity";
        if (hasArm64()) return "arm64: ready";
        if (has32Bit()) return "32-bit: needs translator (not yet)";
        if (abis.isEmpty()) return "Java only: ready";
        return "unsupported ABIs: " + abis;
    }

    synchronized Bitmap icon() {
        if (!iconLoaded) {
            iconLoaded = true;
            icon = BitmapFactory.decodeFile(iconFile().getPath());
        }
        return icon;
    }

    void save() throws IOException {
        Properties p = new Properties();
        p.setProperty("package", packageName);
        p.setProperty("label", label);
        if (launcherActivity != null) p.setProperty("launcher", launcherActivity);
        if (versionName != null) p.setProperty("version", versionName);
        p.setProperty("targetSdk", Integer.toString(targetSdk));
        p.setProperty("abis", String.join(",", abis));
        try (OutputStream out = new FileOutputStream(new File(dir, "meta.properties"))) {
            p.store(out, "ZettaBridge plugin");
        }
    }

    static PluginRecord load(File dir) {
        File meta = new File(dir, "meta.properties");
        if (!meta.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(meta)) {
            p.load(in);
        } catch (IOException e) {
            return null;
        }
        PluginRecord r = new PluginRecord(dir);
        r.packageName = p.getProperty("package");
        if (r.packageName == null) return null;
        r.label = p.getProperty("label", r.packageName);
        r.launcherActivity = p.getProperty("launcher");
        r.versionName = p.getProperty("version");
        try {
            r.targetSdk = Integer.parseInt(p.getProperty("targetSdk", "0"));
        } catch (NumberFormatException e) {
            r.targetSdk = 0;
        }
        for (String abi : p.getProperty("abis", "").split(",")) {
            if (!abi.isEmpty()) r.abis.add(abi);
        }
        return r;
    }
}
