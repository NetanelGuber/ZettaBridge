# Phase 4d: Android Proxy Loading, ART Binding, and Device Acceptance

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load arm32 plugin libraries through ART-visible arm64 proxies, bind their JNI entry points to translated guest code, pass T7 on the phone, and reach Orange Roulette's first unimplemented GLES or asset host call without a JNI error.

**Architecture:** Keep the approved Part 1 design. Import fixes private arm32 library copies once; a plugin class loader returns one `libzbproxy.so` copy per arm32 library; `ZBridge.onProxyLoaded` owns one process-lifetime `LibraryRuntime`, `HostJni`, and `JniEnvBackend`, loads on the calling Java thread's carrier, binds `Java_*`, and runs guest `JNI_OnLoad`.

**Tech Stack:** C++20 core, Android JNI/ART, C11 proxy, Java 17 launcher, arm32 bionic under Dynarmic, CTest, Gradle/AndroidIDE.

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`, especially section 1 and Phase 4 acceptance.

## Global constraints

- Work locally on `codex/phase4c-review-fixes`; do not push or stage `third_party/dynarmic`.
- Repo sources and diagnostics are English and ASCII only.
- No APK repackaging, root dependency, ART hook, executable guest binary, or guest-specific special case.
- Guest code uses the real arm32 linker/bionic and explicit `ZB_GUEST_RTLD_*` values.
- `LibraryRuntime` and `HostJni` are one-per-process and process-lifetime.
- Every behavior change starts with an observed failing test; commit each task separately.
- Device-only failures must be visible on screen/clipboard/file because OxygenOS may drop logcat.

## Decisions locked for this plan

1. **One ABI per plugin.** If arm64 libraries exist, retain the Phase 0 native path. Otherwise prefer `armeabi-v7a`, then `armeabi`, and extract only that directory.
2. **One proxy path per guest library.** The class loader copies the packaged `libzbproxy.so` to `plugins/<pkg>/proxy/lib<name>.so` atomically and returns that unique read-only path.
3. **Fixed runtime layout.** Launcher assets extract to `files/zb/{sysroot,guest}`. The plugin root and guest library basename are derived from the proxy path; `targetSdk` comes from the plugin record established before plugin `Application.onCreate`.
4. **Calling thread is preserved.** Loading, `dlsym`, export binding, and `JNI_OnLoad` use the current Java thread's cached carrier, or `call_on_current` for a nested guest-to-Java-to-load transition.
5. **Class lookup is plugin-scoped.** The active plugin class loader is retained as a global reference for reflection/binding; launcher bridge classes remain parent-first only for `com.zettabridge.core.*`.
6. **ELF mutation is shared C++.** The Python fixer remains a reference/compatibility entry point, while core owns the validated implementation used by host tests, the developer CLI, and launcher JNI.
7. **No silent multi-plugin reuse.** Once the process-lifetime guest runtime starts for one plugin root/targetSdk, an attempt to activate an incompatible plugin in the same `:guest` process fails explicitly. Process pooling is outside Phase 4d.

---

### Task 1: Shared ELF32 import fixups

**Files:**
- Modify: `core/include/zb/elf_fixups.h`, `core/src/elf_fixups.cpp`, `core/CMakeLists.txt`
- Create: `cli/zbfix/main.cpp`, `cli/zbfix/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt`, `tools/run_guest_tests.sh`
- Create/Test: `tests/host/elf_fixups_test.cpp`; modify `tests/host/CMakeLists.txt`

**Produces:** a bounded in-place ARM ELF32 fixer with structured status for absolute `DT_NEEDED`, `DT_TEXTREL`, and `DF_TEXTREL`, plus `zbfix` for developer scripts.

- [x] Create fixture copies covering unchanged ELF, slash and backslash `DT_NEEDED`, marker conversion, flags-only marker insertion, no spare dynamic slot, malformed/truncated headers, wrong class/endian/machine, and idempotence.
- [x] Run the new host test and record RED because the mutation API/CLI does not exist.
- [x] Port the Python algorithm with overflow/range checks before every file offset and preserve file bytes except dynamic words.
- [x] Switch the guest preparation path to `zbfix`; compare its outputs byte-for-byte with the Python implementation on all six Orange Roulette libraries.
- [x] Run `elf_fixups_test`, `elf_loader_test`, and all guest cases; commit `core: share guest ELF import fixups`.

### Task 2: Dynamic export discovery and binding model

**Files:**
- Create: `core/include/zb/elf_symbols.h`, `core/src/elf_symbols.cpp`
- Create: `core/include/zb/jni_loader.h`, `core/src/jni/loader.cpp`
- Modify: `core/include/zb/jni_backend.h`, `core/include/zb/host_jni.h`, `core/src/jni/host_jni_natives.cpp`, `core/CMakeLists.txt`
- Modify: `tests/host/mock_jvm.h`, `tests/host/mock_jvm.cpp`, `tests/host/CMakeLists.txt`
- Create: `tests/host/jni_loader_test.cpp`
- Create: `guest/testlib/zbloadprobe.c`; modify `tools/build_guest.sh`

**Produces:** validated `Java_*`/`JNI_OnLoad` export names from ELF32 and a platform-neutral loader that matches declared native methods, registers one method at a time, and invokes guest `JNI_OnLoad`.

- [x] Add host fixtures for SysV/GNU-hash dynamic symbols, stripped sections, malformed tables, duplicate exports, short/long JNI names, overload suffixes, a missing class, registration failure, supported/unsupported `JNI_OnLoad`, and no `JNI_OnLoad`.
- [x] Observe RED for the absent scanner/loader APIs.
- [x] Parse only bounded file-backed dynamic data; return names, never trust guest `st_value` as the runtime address. Resolve each selected name through guest `dlsym` to retain the Thumb bit.
- [x] Extend the backend with declared-native discovery returning exact descriptors and staticness. Short exports bind every matching declared native; long exports match the decoded argument descriptor. Missing classes are cleared/logged once; other binding failures abort that library load.
- [x] Add a HostJni current-thread loader surface so successful `dlopen`, per-thread `dlerror`, binding, and guest `JNI_OnLoad(JavaVM*, NULL)` share the caller's current/nested carrier.
- [x] Run `jni_loader_test`, `jni_bridge_test`, generator check, and the full host suite; commit `jni: load and bind guest JNI libraries`.

### Task 3: Real ART discovery backend

**Files:**
- Modify: `core/android/jni_env_backend.h`, `core/android/jni_env_backend.cpp`
- Create: `android/t7/java/com/zettabridge/t7/ReflectionSmoke.java`
- Modify: `core/CMakeLists.txt`

**Produces:** exact descriptor construction and declared-native enumeration through the active plugin class loader on real ART.

- [x] Add a Java compile fixture with primitive, reference, array, overloaded, static, and instance native methods; first make an Android-side compile/test seam fail for the absent enumeration API.
- [x] Cache required `ClassLoader`, `Class`, `Method`, `Modifier`, and `Executable` ids/global refs; build exact descriptors (`.` to `/`, arrays preserved) and filter `Modifier.isNative` plus method name.
- [x] Keep pending exceptions for real errors, but clear `ClassNotFoundException` only for the spec's skip case; delete every temporary local reference.
- [x] Compile with `javac` against android-36 and link Android `zbridge`; commit `android: discover plugin native methods through ART`.

### Task 4: Standalone arm64 proxy

**Files:**
- Create: `core/android/zbproxy.c`
- Modify: `core/CMakeLists.txt`
- Create: `tools/check_zbproxy.py`, `tests/proxy/fake_jni.c`

**Produces:** `libzbproxy.so` with no dependency on `libzbridge.so`; its only job is `dladdr` self-identification and `ZBridge.onProxyLoaded(String)` delegation.

- [x] Add a fake-JNI unit for failure/version behavior and a structural checker for exported `JNI_OnLoad`, allowed `DT_NEEDED`, and required ZBridge class/method strings.
- [x] Observe RED before adding the target.
- [x] Implement the proxy with complete JNI exception/local-reference handling; return `JNI_ERR` on failure and guest version (default `JNI_VERSION_1_6`) on success.
- [x] Link the Android proxy and inspect it with `readelf -dWs`; commit `android: add the arm32 library proxy`.

### Task 5: Process-lifetime Android guest runtime

**Files:**
- Modify: `core/android/zbridge_jni.cpp`
- Create: `core/android/guest_jni_runtime.h`, `core/android/guest_jni_runtime.cpp`
- Modify: `core/CMakeLists.txt`
- Create: `android/launcher/app/src/main/java/com/zettabridge/core/ZBridge.java`

**Produces:** Java initialization/fixup calls and native `onProxyLoaded`, backed by one configured `LibraryRuntime + JniEnvBackend + HostJni + JniLoader` graph.

- [x] Add host-testable path/config validation and state-machine cases: first start, repeated same proxy, concurrent different proxies, bad layout, preload failure, second plugin rejection, bind failure, and unsupported JNI version.
- [x] Observe RED for the absent runtime state machine.
- [x] Start `zbhost <targetSdk> libzbjni.so` with `LD_LIBRARY_PATH` containing runtime guest libs and the active plugin lib dir; chain HostJni before start.
- [x] Load `libzbcompat.so` through zbhost's mandatory preload, bind the requested guest library, run `JNI_OnLoad`, memoize success by canonical proxy path, and throw `UnsatisfiedLinkError` with screen-visible detail on failure.
- [x] Expose the shared ELF fixer to import code and retain process-lifetime objects intentionally; compile host seams and Android link; commit `android: connect proxy loads to the guest JNI runtime`.

### Task 6: Launcher ABI extraction, class loading, and bundle

**Files:**
- Create: `android/launcher/app/src/main/java/com/zettabridge/launcher/PluginClassLoader.java`
- Modify: `LoadedPlugin.java`, `PluginStore.java`, `PluginRecord.java`, `GuestRuntime.java`, `LibraryActivity.java`
- Modify: `android/launcher/app/build.gradle.kts`
- Create: `tools/make_launcher_bundle.sh`
- Create: pure-Java/JVM tests or a compile harness under `tests/launcher/`

**Produces:** a launcher APK that packages/extracts the 4.1 MB arm32 sysroot and guest helpers, fixes arm32 imports, delegates bridge classes, and returns unique proxy paths.

- [x] Add filesystem/class-loader contract tests for ABI priority, path sanitization, atomic proxy creation, core-only parent delegation, missing-library fallback, reimport cleanup, and metadata/status.
- [x] Observe RED against the current arm64-only extractor and boot-parent `DexClassLoader`.
- [x] Extract the selected arm32 ABI to the private plugin lib dir, invoke the native fixer before marking import complete, and leave the APK unchanged/read-only.
- [x] Build runtime assets from exactly the 10 sysroot files plus `zbhost`, `libzbcompat.so`, `libzbjni.so`, generated `libGLESv2.so`/`libandroid.so`, `libzbridge.so`, and `libzbproxy.so`; make Gradle consume only ignored `build/launcher` outputs.
- [x] Initialize runtime assets before plugin code, create `PluginClassLoader`, and preserve the existing resource/context fixes.
- [x] Run Java compile checks, bundle validation (filenames, ELF class/machine, size), launcher Gradle build where available, and existing core suites; commit `launcher: route arm32 libraries through ZettaBridge`.

**Task 6 decisions:** ABI priority is arm64-v8a, armeabi-v7a, then armeabi. Imports are
prepared in a sibling staging directory; metadata is written last, the old plugin data directory
is preserved, and the staged directory replaces all other old files. Runtime assets are versioned
and atomically installed under `files/zb`. `PluginClassLoader` is child-first outside platform
classes, delegates only `com.zettabridge.core.*` to the launcher, and creates one immutable proxy
copy per arm32 library. This checkout has no Gradle wrapper or system Gradle, so the portable full
Java compile harness replaces the unavailable local APK build; AndroidIDE/device packaging remains
part of Task 7.

### Task 7: T7 on real ART

**Files:**
- Create: `android/t7/` project/test sources and `tools/make_t7_bundle.sh`
- Reuse: `guest/zbjni`, `guest/testlib/zbjniprobe.c`; create `guest/testlib/zbt7probe.c`
- Modify: `docs/phase4-device-test.md`

**Produces:** an installable diagnostics app that exercises the real JNIEnv backend without depending on the Phase 0 launcher.

- [ ] Build a minimal plugin class loader/proxy harness and Java model covering all argument/return types, all three `Call*` forms, strings, arrays/release modes, globals/weaks, exceptions, `RegisterNatives` from guest `JNI_OnLoad`, guest pthread attach, two Java callers, and nested Java/guest calls.
- [ ] Run compile/bundle structural checks locally and record expected on-screen checkpoints.
- [ ] Have the user install/run on the OnePlus 13; collect clipboard/file diagnostics, fix failures through focused host regressions where reproducible, and repeat until every T7 checkpoint passes.
- [ ] Commit `android: add the Phase 4 JNI device test` after device acceptance.

### Task 8: Orange Roulette Phase 4 smoke launch

**Files:**
- Test: Orange Roulette through `android/launcher/` on the target device
- Modify: `docs/phase0-launcher.md`, `docs/phase4-device-test.md`, `CLAUDE.md`, `AGENTS.md`, `README.md`

**Produces:** the first end-to-end 32-bit launcher milestone.

- [ ] Import the untouched Orange Roulette APK; verify all six private arm32 copies and expected fixups (TEXTREL in ApplicationMain/lime/openal; absolute NEEDED in ApplicationMain/lime).
- [ ] Launch through the plugin Activity and verify six proxy loads, successful `JNI_OnLoad` for lime/openal, 20 native registrations, and `HXCPP.main()` reaching the first logged GLES or `AAsset*` host call with no JNI error.
- [ ] If the process fails, preserve the first error on screen/clipboard/file and add a focused regression before changing shared code.
- [ ] Re-run host 18/18, guest 9/9, generator check, repeated JNI bridge, Android links, T7, and launcher build.
- [ ] Record device evidence and remaining Phase 5 boundary; commit `docs: mark Phase 4 complete`.

## Acceptance

- Host and guest suites remain green; generated JNI/stub files are current.
- Android arm64 `zbridge`, `zbrun`, and `zbproxy` link; launcher APK contains only arm64 host code and arm32 data assets/private extracted libraries.
- T7 passes against real ART on the OnePlus 13.
- Orange Roulette reaches the first intentionally unimplemented GLES or `AAsset*` call after all six guest libraries, both guest `JNI_OnLoad`s, and all 20 native registrations succeed.
- No special case names Orange Roulette, Haxe, lime, or its package in core/loader behavior.

## Deferred beyond Phase 4d

- GLES passthrough/first frame (Phase 5), input/audio/assets/playability (Phase 6).
- Carrier pooling and idle release, `ZB_JNI_STATS`, mirrored foreign direct buffers, guest-env leak on attach-without-detach, and the minor missing-HostJni diagnostic wording.
- `System.load(path)`, guest-created class loaders, `NativeActivity`, and simultaneous multiple plugin runtimes in one process.

## Self-review

- Every Part 1 loading step maps to Tasks 2-6; T7 and Orange acceptance map to Tasks 7-8.
- Producer/consumer order is explicit: fixups and symbol model precede Android runtime; runtime/proxy precede launcher; launcher precedes device tests.
- The plan uses current Phase 4b/4c interfaces and preserves the approved per-method registration, `!` handling, host shorties, and never-bound-only slot reuse rules.
- No task requires editing generated JNI files by hand or staging the Dynarmic patch.
