package com.zettabridge.core;

import java.io.IOException;

/**
 * JNI surface of libzbridge.so in the launcher's :guest process.
 *
 * <p>Order for an arm32 plugin, all in one :guest process:
 * <ol>
 *   <li>Import: {@link #fixGuestLibrary} on every extracted arm32 library before the import is
 *       marked complete.</li>
 *   <li>Launch, before plugin code runs (before the plugin Application is created):
 *       {@link #activatePlugin} with the plugin root, its targetSdk and its class loader.</li>
 *   <li>Plugin code calls System.loadLibrary; the plugin class loader returns
 *       filesDir/plugins/&lt;pkg&gt;/proxy/lib&lt;name&gt;.so, and that proxy's JNI_OnLoad calls
 *       {@link #onProxyLoaded}.</li>
 * </ol>
 *
 * <p>Runtime layout (fixed): filesDir/zb/sysroot/system/..., filesDir/zb/guest/zbhost,
 * filesDir/zb/guest/lib/{libzbcompat.so,libzbjni.so,...}, filesDir/plugins/&lt;pkg&gt;/lib/lib&lt;name&gt;.so.
 *
 * <p>Failures are final for the process. ART replaces the UnsatisfiedLinkError thrown here by a
 * generic "JNI_ERR returned from JNI_OnLoad" and never calls JNI_OnLoad again for the same path;
 * the guest runtime is started once and never restarted; only one plugin can be active. Show
 * {@link #lastLoadError} (or {@link #loadError}) on screen, and restart the :guest process before
 * retrying or switching plugins.
 */
public final class ZBridge {
    static {
        System.loadLibrary("zbridge");
    }

    private ZBridge() {}

    /**
     * Blocks until the guest exits.
     *
     * @param sysroot directory holding system/bin/linker and system/lib (arm32 bionic)
     * @param argv argv[0] is the absolute path of the guest executable
     * @param envp guest environment ("NAME=VALUE"), or null to pass this process's environment
     * @return the guest exit status, or 128 + signal for a guest crash
     */
    public static native int runExecutable(String sysroot, String[] argv, String[] envp);

    /**
     * Binds this process's guest JNI runtime to one plugin. Calling it again with the same plugin
     * root, targetSdk and class loader is a no-op.
     *
     * @param pluginRoot filesDir/plugins/&lt;pkg&gt; (symlinks are resolved)
     * @param targetSdk the plugin's targetSdkVersion, passed to the guest linker
     * @param classLoader the plugin class loader; retained for the life of the process
     * @throws IllegalStateException when the layout is incomplete, or another plugin, targetSdk
     *     or class loader is already active in this process
     */
    public static native void activatePlugin(String pluginRoot, int targetSdk, ClassLoader classLoader);

    /**
     * Called by libzbproxy.so from JNI_OnLoad. Loads the arm32 library that belongs to the proxy,
     * binds its Java_* exports and runs its JNI_OnLoad on the calling thread. Results are memoized
     * by canonical proxy path.
     *
     * @return the guest JNI version (JNI_VERSION_1_2, 1_4 or 1_6)
     * @throws UnsatisfiedLinkError with the detailed reason
     */
    public static native int onProxyLoaded(String proxyPath);

    /** The stored failure message for a proxy path, or null if it did not fail. */
    public static native String loadError(String proxyPath);

    /** The most recent proxy load failure message, or null. */
    public static native String lastLoadError();

    /**
     * Applies the arm32 import fixups (absolute DT_NEEDED to basename, DT_TEXTREL marker) in place.
     *
     * @return "unchanged", "changed: ..." or "skipped: reason" for a file that is not an ARM ELF32
     *     shared library
     * @throws IOException for a malformed ELF file or an I/O error
     */
    public static native String fixGuestLibrary(String path) throws IOException;
}
