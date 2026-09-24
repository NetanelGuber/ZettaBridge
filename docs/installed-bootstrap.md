# Installed package bootstrap (Step 04)

This is the version 1 bootstrap contract for a normal ARM64 installed package. The
Step 04 probe in `android/launcher/step04probe` is a standalone test package. It
does not use the ZettaBridge launcher, a plugin class loader, shared UID, or root.
Step 05 must incorporate this contract into generated output APKs; the probe is
not an APK converter.

## Package layout and activation

- The package includes `lib/arm64-v8a/libzbridge.so` and one copy of the ARM64
  `libzbproxy.so` binary under each expected ARM32 library name. Native libraries
  must be extracted so `dladdr` returns a real path under `nativeLibraryDir`.
- Read-only ARM32 files are extracted under the package's own `filesDir/zb`:
  `sysroot/system/...`, `guest/zbhost`, `guest/lib/...`, and
  `app/lib/lib<name>.so`. The Android linker never sees `app/lib` as host code.
- A bootstrap class named `com.zettabridge.core.ZBridge` loads `zbridge` and
  calls `activateInstalled(filesDir, nativeLibraryDir, targetSdk, classLoader)`
  before the first guest `System.loadLibrary`. The values come from that
  package's `Context` and `ApplicationInfo`, not a launcher or root service.
  Repeating the same activation is allowed. A different configuration in the
  same process fails. Each Android process activates independently.
- `libzbproxy.so` discovers its installed path with `dladdr`. The bridge accepts
  only `lib<name>.so` in the activated native library directory and maps it to
  `filesDir/zb/app/lib/lib<name>.so`. The proxy's `JNI_OnLoad` then invokes the
  bridge's `onProxyLoaded`. The library-mode guest starts once per process,
  preloads `libzbjni.so`, uses the ARM32 sysroot/linker, and searches only its
  guest runtime and app library directories. The existing guest `dlopen/dlsym`
  and JNI loader bind `Java_*` and `RegisterNatives` methods to ART.
- The backend is the embedded A32/T32 translator. JIT code caches are in
  process memory; this step writes no persistent JIT cache. Extracted assets
  and diagnostics stay in package-private storage. No package path depends on
  the manager, launcher, `/data/local/tmp`, or a root-owned directory.

## Lifetime and diagnostics

The bridge and guest threads live for the Android process lifetime. A failed
start or proxy load is final for that process; ART also remembers failed native
loads. The caller can read `lastLoadError()` and the runtime report. The probe
persists `step04-result-<process>.txt` and `step04-runtime-<process>.txt` under
its private files directory, including the package UID, process name, library
loads, JNI calls and failures. Force-stop or process death releases the runtime;
a new process can activate again. The host state-machine test covers recursive
same-proxy rejection, concurrent and repeated loads, failed start and failed
load; the device probe covers two sequential libraries in two processes and a
Java callback from guest `JNI_OnLoad`.

## Reproduce the proof

Use the pinned WSL/NDK setup in `docs/development.md`, then run:

```sh
tools/build_guest.sh
ninja -C build/android-arm64 zbridge zbproxy zbjni_reflection_compile_test
tools/make_launcher_bundle.sh
tools/make_step04_probe_bundle.sh
cd android/launcher
ANDROID_HOME="$ANDROID_SDK_ROOT" ./gradlew :step04probe:assembleDebug
```

Install only the probe package with `adb install -r` and start
`com.zettabridge.step04/.ProbeActivity`. Start
`com.zettabridge.step04/.ProbeService` for the second process. Read the private
result files with `adb shell run-as com.zettabridge.step04 cat
files/step04-result-main.txt` and the corresponding `second` file. A result
marked `PASS` is the device acceptance evidence. The debug fixture stays
installed unless the user chooses to remove it. No manager update is needed.
