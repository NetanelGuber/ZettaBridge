# AGENTS.md: handoff for Codex (2026-09-14)

Read `CLAUDE.md` first. It holds the architecture, gotchas, build commands and working
conventions, and all of it applies to you. The user chats in Russian; repo files are
English and ASCII only.

## State

- **Phases 1-3 done.** T1-T6 pass: host tests, and the guest suite in Termux and inside
  an app process on the OnePlus 13. Branch `phase1-zbrun`.
- **Phase 0 launcher works on the phone.** arm64 plugins launch, including an app with
  Firebase/AdMob.
  - It lives on branch `worktree-agent-a79da63fd7f33c735`, worktree
    `.claude/worktrees/agent-a79da63fd7f33c735`, code in `android/launcher/`.
  - The phone copy is `/sdcard/AndroidIDEProjects/ZettaBridge`; the user builds it in
    AndroidIDE.
  - **Todo 1:** merge that branch into `phase1-zbrun` (only `android/launcher/**`,
    `docs/phase0-launcher.md` and one CLAUDE.md bullet should conflict), then remove
    the worktree.
- **Part 1 (JNI bridge) spec approved:**
  `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`. Follow it, and do not
  redesign without asking the user.
- **Local commits only.** Pushing to GitHub is done together with the user later.

## Next: Phase 4 plan and implementation

**Todo 2:** write `docs/superpowers/plans/2026-09-14-phase4-jni-bridge.md`, then implement
in this order. Each step ends with a passing test and a local commit.

1. **`tools/gen_jni.py`.** Parse `JNINativeInterface` / `JNIInvokeInterface` from NDK
   `jni.h`. Emit the slot table for `guest/zbjni` and the host-call list, plus a config
   marking slots "guest C" or "host stub" (performance fallback; all "guest C" for now).
2. **`core/src/jni/mangle.cpp` + `jni_mangle_test`.** `Java_*` decoding: `_1` `_2` `_3`
   `_0xxxx`, `__sig`.
3. **`core/src/jni/handles.cpp` + `jni_handles_test`.** 32-bit handles, low 2 bits kind
   (01 local, 10 global, 11 weak), local frames, append-only method/field id table,
   invalid handle -> `FatalError`.
4. **`core/src/jni/native_call.cpp` + `thunks.S` + `jni_abi_test`.**
   - 16384-entry thunk pool (`movz x16,#i; b zb_native_common`).
   - AAPCS32 softfp layout from shorty: even pair for J/D, stack spill after a 64-bit
     argument, return values.
   - Table-driven tests including `(IFFIFF)I` and `(J)V`.
5. **Library mode of the guest process.**
   - `guest/zbhost/zbhost.c`: parks, serves `dlopen`/`dlsym`/call.
   - Host-to-guest call helper on `GuestThread` (the `svc #0x5AFFFF` return), plus
     carriers per part 3 spec "Threads".
6. **`guest/zbjni/zbjni.c` -> `libzbjni.so`** (arm32).
   - All 229 + 6 slots.
   - `...` / `va_list` -> `jvalue[]` via `va_arg` and a shorty cache.
   - Buffers via guest `malloc` + `Region` host calls, with the release modes 0 /
     `JNI_COMMIT` / `JNI_ABORT`.
7. **`core/src/jni/host_jni.cpp`.** About 70 flat host calls onto a `JNIEnv` interface.
   - **Mock JNI backend** (toy Java model) so the guest test `jni_mock_dynamic` runs
     under `zbrun` on this machine with no ART.
8. **`android/proxy/zbproxy.c` -> `libzbproxy.so`** plus
   `ZBridge.onProxyLoaded(String)` in `core/android/zbridge_jni.cpp` and
   `core/src/jni/loader.cpp` (dlopen, bind `Java_*` via `RegisterNatives`, guest
   `JNI_OnLoad`).
   - **Launcher change:** the plugin class loader delegates `com.zettabridge.core.*` to
     the launcher loader, and `findLibrary` returns proxy copies for `armeabi*` libs.
9. **Device test T7** (extend `android/t6/project`).
   - Test dex via `d8`, a `BaseDexClassLoader` with `findLibrary` -> proxy, arm32
     `libjniprobe.so`.
   - Coverage list is in spec section 4.
   - Produce the phone project like `tools/make_t6_bundle.sh` and ask the user to build
     and run it.
10. **Acceptance.** T7 passes, then the Orange Roulette smoke test (spec "Phase 4
    acceptance").

## Practical notes

- **Build and test:**
  ```
  ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/build_guest.sh; tools/run_guest_tests.sh
  ```
- **Android core build:** see CLAUDE.md. Android binaries cannot run on this machine,
  so device tests go through the user.
- **Java compile check:** `javac` against `~/android-sdk/platforms/android-36/android.jar`.
  The launcher also needs a stub of `org.lsposed.hiddenapibypass.HiddenApiBypass`.
- **OxygenOS drops third-party app logs in logcat.** Report errors on screen, to the
  clipboard, or to a file (see `android/launcher/.../Diagnostics.java`).
- **Every app manifest must remove the `LogWireInitializer` provider** that AndroidIDE
  injects (see CLAUDE.md).
- **Commit messages:** plain English, imperative subject line.
