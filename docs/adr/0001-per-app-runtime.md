# ADR 0001: Per-app converted APK with an embedded ARM64 runtime

**Status:** Accepted; live device baseline captured for the synthetic architecture spike

**Date:** 2026-09-23
**Decision owners:** User and implementation agent

## Decision

Use an offline-converted APK that contains the ARM64 ZettaBridge runtime and ARM64
proxy libraries. Keep ARM32 libraries in a guest-only area. Android installs the
converted output as a normal package; ART runs its Java code and the translated
ARM32 libraries run inside that package's ordinary app process and Android-assigned
UID. Root is restricted to explicitly requested manager operations.

The first implementation proof uses one synthetic APK and one synthetic ARM32 JNI
library. The first product input target is a standalone APK or a validated complete
set of APK splits. AAB conversion is excluded. A single APK is sufficient for the
first architecture proof.

## Why this model

The current plugin/shortcut model runs guest components inside the launcher's UID,
process, and class loader. It emulates package metadata through launcher hooks and
does not install the guest as its own PackageManager package. That fails the goal of
a normal installed package with its own UID, permission prompts, and Android-managed
components. The existing launcher runtime remains a source of reusable native bridge
code, but its plugin component virtualization is not the installed-app architecture.

The converted package instead owns its Java bootstrap, private runtime files,
proxies, and per-process guest runtime configuration. It keeps the source package
name by default. Conversion changes the signing certificate; it cannot preserve or
recreate the source private key. A stable personal key signs every output and future
update. If a source-signed package with the same package name is installed, Package
Manager must reject the different signer. Stop and preserve the source app and its
data; never silently uninstall it, replace it, or imply that root bypasses signing.

## Data flow

1. The unprivileged manager reads a user-selected APK or validated complete split
   set. It analyzes input without loading or executing its code.
2. The unprivileged conversion pipeline writes a private staging tree, moves ARM32
   ELF files to guest-only assets, adds ARM64 runtime and proxy libraries, and
   injects the bootstrap configuration. It verifies package metadata, ABI placement,
   hashes, and structure before signing and atomically publishing the output.
3. After an explicit user choice, the manager's narrow root backend invokes the
   Android PackageManager installation action. Package Manager assigns the installed
   package its own UID and sandbox. Root does not launch the package.
4. Android starts the installed app under that UID. Its bootstrap installs the
   bundled runtime into app-private storage, establishes per-app/per-process paths,
   and initializes the bridge before guest native code can load.
5. ART loads an ARM64 proxy for a requested native library. The proxy's
   `JNI_OnLoad` reaches the package-local bootstrap and initializes or reuses the
   process-lifetime `libzbridge.so` runtime.
6. The runtime opens the corresponding ARM32 library with the packaged ARM32
   sysroot/linker, translates it with Dynarmic, runs guest `JNI_OnLoad` and
   `RegisterNatives`, and dispatches guest JNI calls back to this process's ART.
7. Runtime state, JIT cache, extracted guest files, and diagnostics remain under the
   installed app's private directory. The guest receives no root service or host
   pointer access.

## Trust, identity, and storage boundaries

| Boundary | Decision |
| --- | --- |
| Untrusted input | Parse and transform as the normal manager UID. Never execute imported code during analysis or conversion. Validate archive paths, file counts, sizes, hashes, and ELF/manifest structures. |
| Root manager | KernelSU is the initial provider target. Root may verify the provider, stage explicitly selected input, invoke an explicitly authorized PackageManager install/update/remove operation, and collect explicitly requested diagnostics or backups. Use fixed operations and validated paths. |
| Installed app | Run Java, ARM64 proxies, translated ARM32 code, and bootstrap only as the UID assigned by Package Manager. Root denial or loss must not turn into root execution. |
| Runtime files | Keep the guest sysroot read-only. Put extracted guest libraries, JIT cache, reports, and app data in app-private locations. No root-only path is permitted at runtime. |
| Signing and updates | Use one stable personal signing key. Verify the output signer and record its fingerprint and input/output hashes. A source certificate mismatch is a hard update conflict; preserve the installed source package and data. |
| Storage | ADB reports 414 GB available on the data filesystem. This is filesystem free space, not an app-specific quota. Conversion must still budget source/split bytes, private staging, output, installed package, runtime bundle, and rollback metadata; publish only after verification and clean failed staging safely. |

Root must not modify `ro.dalvik.vm.*`, ART, the system image, SELinux policy, or
system partitions. The spike must pass while SELinux is enforcing. If execution
requires permissive SELinux or guest code running as UID 0, this architecture is a
no-go.

## Target device baseline and evidence

| Property | Current evidence | State |
| --- | --- | --- |
| Device / SoC | Google Pixel 11 Pro XL (`kodiak`), Google Tensor G6. Live ADB serial: `67161FDDV0011Q`. | Captured 2026-09-23; first per-app spike target. The OnePlus 13 acceptance records are from the original project owner, not this fork owner. |
| Host ABI | `ro.product.cpu.abi=arm64-v8a`; `ro.product.cpu.abilist=arm64-v8a`. | Captured 2026-09-23; Android advertises only the ARM64 app ABI. This is not a direct CPU execution-state probe. |
| Android release / API / build fingerprint | Android 17, API 37, build `google/kodiak/kodiak:17/CD1A.260905.001.B1/16238327:user/release-keys`; security patch `2026-09-01`. | Captured 2026-09-23. |
| Guest ABIs | Initial target is `armeabi` and `armeabi-v7a`, as defined by this plan. | Scope decision; validate per input ELF. |
| Root provider | Package `me.weishu.kernelsu` is installed; `su -v` reports `3.3.0:KernelSU`. | Captured 2026-09-23. No root command or grant prompt was invoked; authorization behavior belongs to Step 03. |
| SELinux / build posture | `getenforce=Enforcing`, `ro.secure=1`, `ro.debuggable=0`, `ro.boot.verifiedbootstate=green`. | Captured 2026-09-23. The synthetic runtime/JIT proof must still pass under this state. |
| Graphics | `ro.opengles.version=196610` (Android encoding for GLES 3.2); `android.hardware.opengles.aep` is advertised. `ro.hardware.egl=powervr`, `ro.hardware.vulkan=powervr`; Package Manager advertises Vulkan level 1 and version `4210688` (1.4.0). SurfaceFlinger reports a Vulkan device initialized. | Captured 2026-09-23. These are device/framework capability observations, not a probe of an app GL context. `GL_VENDOR`, `GL_RENDERER`, and extension strings were not exposed by the collected dumpsys output. Earlier GLES 2.0 Orange Roulette rendering is historical evidence recorded by the original project owner on their OnePlus 13. |
| Storage | `df -h /data/user/0`: `/dev/block/dm-115`, 462G size, 49G used, 414G available (11%), mounted at `/mnt/pass_through/0/emulated`. | Captured 2026-09-23. App-specific quota and conversion staging headroom were not measured. |
| ADB availability | `adb devices -l` lists serial `67161FDDV0011Q` as `device`, model `Pixel_11_Pro_XL`, product/device `kodiak`. | Captured 2026-09-23. |

Do not treat historical launcher acceptance as evidence that a converted standalone
APK can load its proxy, access its app-private guest sysroot, or run NativeActivity.
The inventory above closes the Step 00 device-baseline gate. Root grant behavior is
reserved for Step 03; app-context GL strings and app-private storage checks require
the synthetic package and remain implementation acceptance tests. No converted APK,
guest sysroot, JIT, JNI proxy, or NativeActivity was run during Step 00.

The inventory used read-only ADB commands: `adb devices -l`, `getprop`, `getenforce`,
`pm list packages`, `su -v`, `pm list features`, `df -h /data/user/0`, and
`dumpsys SurfaceFlinger`. No `su -c` command or root grant prompt was requested.

## Existing-code reuse and launcher assumptions

Source review was performed at repository baseline
`4acdf51c11118b1d9d04c2c117feab249ea51072`:

| Existing code | Reuse / constraint |
| --- | --- |
| [`zbproxy.c`](../../core/android/zbproxy.c) | Reuse its small ARM64 `JNI_OnLoad` proxy contract. It currently calls `com.zettabridge.core.ZBridge.onProxyLoaded`; the installed package needs a package-local bootstrap with that contract or an intentionally revised one. |
| [`guest_jni_runtime.h`](../../core/android/guest_jni_runtime.h), [`guest_jni_runtime.cpp`](../../core/android/guest_jni_runtime.cpp), [`proxy_runtime.cpp`](../../core/src/jni/proxy_runtime.cpp) | Reuse the ART backend, `GuestJniEngine`, guest loader, JNI dispatch, and proxy load state machine. `GuestJniRuntime` intentionally lives for the app process, retains the class loader, and currently supports one active guest root/target SDK/class loader per process. That maps to one installed package per process, with each Android process bootstrapping independently. |
| [`ZBridge.java`](../../android/launcher/app/src/main/java/com/zettabridge/core/ZBridge.java), [`zbridge_jni.cpp`](../../core/android/zbridge_jni.cpp) | Reuse the native boundary and error/report concepts, but the current Java class is bundled in the launcher and statically loads `zbridge`. The converted APK must include and initialize its own class/bootstrap. |
| [`RuntimeBundle.java`](../../android/launcher/app/src/main/java/com/zettabridge/launcher/RuntimeBundle.java), [`make_launcher_bundle.sh`](../../tools/make_launcher_bundle.sh) | Reuse the generated asset manifest and atomic, private-directory installation pattern. The launcher currently installs under its own `filesDir/zb`; the converted package must install under its own app data directory and prove ordinary-UID access. |
| [`PluginClassLoader.java`](../../android/launcher/app/src/main/java/com/zettabridge/launcher/PluginClassLoader.java), [`GuestRuntime.java`](../../android/launcher/app/src/main/java/com/zettabridge/launcher/GuestRuntime.java), [`PluginContext.java`](../../android/launcher/app/src/main/java/com/zettabridge/launcher/PluginContext.java), [`PackageManagerHook.java`](../../android/launcher/app/src/main/java/com/zettabridge/launcher/PackageManagerHook.java) | Do not depend on these for installed identity. They delegate bridge classes to the launcher, route `System.loadLibrary` through plugin proxy copies, replace guest components with launcher stubs, and virtualize package queries. A converted app must use Android's real components and package registration. |

The current code is evidence that the native proxy/JNI engine exists; it is not
evidence that its launcher-specific bootstrap is already suitable for an installed
APK.

## Smallest synthetic proof and go/no-go checks

Build a minimal Java Activity APK with one ARM64 `libstep00probe.so` proxy and one
ARM32 guest `libstep00probe.so`. The ARM32 library implements `JNI_OnLoad`, registers
one native method, calls a Java callback, and returns a deterministic value. Place
the guest ELF and minimal sysroot outside Android's host native-library directory.
Use no commercial APK or app data.

The spike passes only when all checks hold:

1. The signed output preserves the chosen package name and is accepted by Package
   Manager as an ARM64-installable app. Its verified personal signer fingerprint is
   stable. An installed source-signed package conflict is detected before mutation.
2. Package Manager shows the output as a normal app with a non-root assigned UID.
   Its process reports that same UID and starts with SELinux enforcing.
3. `System.loadLibrary("step00probe")` loads the ARM64 proxy. Its `JNI_OnLoad`
   reaches the package-local bootstrap, loads the ARM32 guest library exactly once,
   and reports the guest `JNI_OnLoad` result.
4. The registered ARM32 method returns the expected value and completes the Java
   callback on ART. No ARM32 ELF is offered to Android's host linker.
5. The guest linker can read the bundled, read-only ARM32 sysroot and guest files
   from app-private storage, and Dynarmic can use its code cache with SELinux
   enforcing. The process never requests or receives UID 0.
6. Input APK/splits remain byte-for-byte unchanged; a failed conversion/install does
   not replace source input or remove an existing installed package/data.

After the Java/JNI proof, run a second synthetic NativeActivity package using
`android.app.lib_name`. Its gate is: Android starts the ARM64 entry proxy; translated
ARM32 `ANativeActivity_onCreate` is reached; the fixture observes create, a window,
input, pause/resume, and destroy through real app-UID APIs. No named game is needed.

Stop and revisit the ADR if the proxy cannot be loaded by ART in an ordinary package,
bootstrap cannot run before the first native load, app-private sysroot/JIT access
fails under enforcing SELinux, Package Manager cannot accept the converted package
without system modification, NativeActivity requires a system-wide bridge, or any
guest execution requires root. Do not proceed to broad conversion work until the
first Java/JNI proof passes. NativeActivity is a separate architecture gate, not a
claim that its APIs are implemented now.

## Biggest unresolved risk

The biggest risk is installing and initializing the bridge early enough inside an
ordinary converted package for ART's first `System.loadLibrary` and NativeActivity
entry paths, while staying within that package's app UID, linker namespace, private
storage, and enforcing SELinux policy. The current proxy works through the launcher
class loader; conversion must provide an equivalent package-local bootstrap without
the launcher's instrumentation or class delegation. The synthetic Java/JNI APK is
the smallest experiment that can resolve the core proxy/bootstrap part of this risk.

## NativeBridgeCallbacks comparison

AOSP's Native Bridge is an ART/runtime integration: ART loads and initializes a
configured bridge, asks it to load foreign-ISA libraries, and obtains JNI trampolines;
the guest libraries use a separate linker/environment. That is a good system-wide
abstraction and a useful fallback comparison, especially for automatic native-load
and NativeActivity routing.

It is not selected for this per-app deliverable. It would require configuring and
supporting a bridge at the device ART/runtime level rather than carrying a
self-contained runtime in each converted APK. Changing system bridge properties,
ART, or system images is outside this root boundary and would affect device-wide
native loading. The current decision is therefore an ARM64 proxy plus a package-local
bootstrap. This comparison is based on the AOSP callback contract, not on a live
OnePlus Native Bridge experiment.

References: [AOSP Native Bridge overview](https://android.googlesource.com/platform/art/%2B/refs/heads/main/libnativebridge/README.md), [AOSP callback interface](https://android.googlesource.com/platform/art/%2B/cea2f596f9/libnativebridge/include/nativebridge/native_bridge.h), [Android app signing](https://developer.android.com/studio/publish/app-signing), and [Android application sandbox](https://source.android.com/docs/security/app-sandbox).
