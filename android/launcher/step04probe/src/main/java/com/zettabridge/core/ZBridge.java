package com.zettabridge.core;

/** Stable JNI bootstrap surface packaged into an ordinary installed app for Step 04. */
public final class ZBridge {
    static { System.loadLibrary("zbridge"); }
    private ZBridge() {}

    public static native void activateInstalled(String filesDir, String nativeLibraryDir,
            int targetSdk, ClassLoader classLoader);
    public static native int onProxyLoaded(String proxyPath);
    public static native String lastLoadError();
    public static native boolean setReportFile(String path);
    public static native String runtimeReport();
}
