# AGENTS.md: handoff for Codex (2026-09-14)

Read `CLAUDE.md` first. It holds the architecture, gotchas, build commands and working
conventions, and all of it applies to you. The user chats in Russian; repo files are
English and ASCII only.

## State

- **Phases 1-3 done.** T1-T6 pass: host tests, and the guest suite in Termux and inside
  an app process on the OnePlus 13. Branch `phase1-zbrun`.
- **Phase 0 launcher works on the phone** and is merged into `phase1-zbrun`
  (`android/launcher/`, phone copy `/sdcard/AndroidIDEProjects/ZettaBridge`).
- **Part 1 (JNI bridge) spec approved:**
  `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`. Follow it; do not redesign
  without asking the user.
- **Phase 4a (JNI host units) done.** All 10 host tests and all 9 guest tests pass.
- **Phase 4b done.** All six tasks (nested call frame, stop dispatch, `zbhost`
  protocol, service-thread runtime, carriers, regression/docs) are committed and
  reviewed. Host tests are 15/15. Phase 4c followed.
- **Phase 4c done (2026-09-15).** Guest `JNIEnv`/`JavaVM`, host JNI calls against
  `JniBackend`, Java -> guest dispatch, `RegisterNatives`, attach/detach; committed directly
  to `phase1-zbrun` (see "Phase 4c done" below). Host tests are 18/18. Next is plan 4d.
- **Phase 4d Tasks 1-6 done.** ELF fixups/symbol scanning, JNI loader, ART backend, standalone
  proxy, process-lifetime runtime, and launcher integration are implemented. Next is Task 7 (T7
  on real ART), then the two recorded ART discovery review fixes before Task 8.
- Work continues on local branch `codex/phase4d-launcher`, based on `phase1-zbrun`.
- Commit locally after every task and update this file. Push only with the user's agreement.

## Phase 4a completed (JNI host units)

Plan: `docs/superpowers/plans/2026-09-14-phase4a-jni-host-units.md`. Its 6 TDD tasks
were implemented in order, with the full host suite run before each local commit.

**Progress (2026-09-14):**

| Task | State | Commits |
|---|---|---|
| 1. `Java_*` name decoding | done, reviewed | `5eeada8`, `96250bc` (strict ART-canonical decoding after review; the plan's Task 1 code was synced) |
| 2. Signature -> shorty | done, reviewed | `cbfe969`, `7558c25` (strict class-name scan, 255-dimension limit; plan synced), `44362e8` (docs) |
| 3. AAPCS64 -> AAPCS32 marshaling | done, reviewed | `fe12334`, `69051d4` (strict invalid-shorty failure and additional ABI coverage) |
| 4. Handle tables | done | `07c2443` |
| 5. Thunk pool (`thunks.S`), dispatcher, slots | done | `1ffb543` |
| 6. Regression run + docs | done | `3d7d15e` |

All Phase 4a review notes were folded into the implementation, tests, plan, and spec.

## Phase 4b done

The executable TDD plan is
`docs/superpowers/plans/2026-09-14-phase4b-library-runtime.md`. All six tasks are
complete. The plan's "Review decisions" table (D1-D14) says where each review decision
lands; the specs were amended to match.

| Task | State | Commit |
|---|---|---|
| 1. Nested host-to-guest call frame | done, reviewed, review fixes | `d0708d8`, `a15b86c` |
| 2. Reusable Process stop dispatch | done, reviewed, review fixes | `fcaef77`, `a15b86c` |
| 3. Fixed `zbhost` service protocol | done, reviewed | `49608ec` |
| 4. Service-thread library runtime | done, reviewed, review fix | `d3b7119`; P1 fix `5e21137` |
| 5. Carrier leases and guest-tid routing | done | pre-fix `1c3d145`; carriers `75015d9`; review fix `54c6776` |
| 6. Regression, Android link, and docs | done | this commit |

`a15b86c` fixed: IT/E bits cleared on call entry, `call_depth`, thread exit inside a call
ends the host process, stray or wrong-`sp` return svc is SIGILL, shared `after_stop`,
code cache size parameter, guest tid in crash reports, `_Exit` in `check.h`, and the
extended `guest_call_test`.

Phase 4a hardening after review (thunk abort/CFI/exception barrier, handle serials and LIFO
reclaim) landed on this branch before Task 4.

The Task 1 review note (observable register mutation, handler-false after execution) is
done in `a15b86c`.

The Superpowers SDD scratch ledger and generated Task 1-6 briefs are under
`.superpowers/sdd/2026-09-14-phase4b-library-runtime/` in the Phase 4b worktree. The
directory is intentionally git-ignored.

The isolated worktree uses an ignored `sysroot` symlink. Its Dynarmic submodule has
the same five-file uncommitted baseline patch as the main checkout. That patch is
required: a clean recorded Dynarmic revision fails `fault_pc_test` and encounters an
unsupported `ldab` in the Android linker. Never stage the submodule pointer or modify
that patch as part of Phase 4b.

Last verified at `54c6776` (Task 6 regression):

```text
ctest --test-dir build/host                  15/15 PASS
tools/build_guest.sh                         PASS
tools/run_guest_tests.sh                     all 9 cases PASS (Orange Roulette APK
                                              symlinked into the worktree)
library_runtime_test, 20-run loop            0 failures
Android arm64 zbridge + zbrun                link OK
```

## After Phase 4b: plans 4c and 4d

Write each plan after 4b lands, using the real interfaces. The scope and the review
notes (per-method `RegisterNatives`, the `!` prefix, host-computed shorties, slot
release only for never-bound slots) are at the end of the 4a plan under "Following
plans". Use the same format as the 4a plan: TDD tasks with complete code.

- **4c:** generated guest `JNIEnv`, host JNI backend, and mock-JNI host test.
- **4d:** ART proxy loading, per-method registration, launcher integration, T7, and
  the Orange Roulette phone smoke test. This is the first end-to-end 32-bit launcher
  milestone.

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

## Phase 4b status (2026-09-14, Task 6 complete)

*(The 2026-09-15 "Claude limit reached mid-task" handoff that used to be here is
obsolete: the Task 4-5 work it described as a prototype is committed at `d3b7119`,
`5e21137`, `1c3d145`, `75015d9`, and `54c6776`, and Task 6 below closes out the plan.)*

- **Task 4:** `d3b7119`. Its P1 fix (retire the process signal target safely) is `5e21137`.
- **Task 5:** `1c3d145` (clear thread-local before freeing cloned threads) and `75015d9`
  (carriers). The review fix (tkill/tgkill post under the registry lock) is `54c6776`.
- **Task 6:** this commit (regression, Android link, docs).
- **Verification:** 15/15 host tests, a 20-run repeated `library_runtime_test` loop with
  0 failures, the guest suite passes (Orange Roulette's `or_dlopen_dynamic` included),
  and the Android `zbridge`/`zbrun` targets link.
- **Next:** plan 4c (guest `JNIEnv`). Then merge the `codex/*` branches into
  `phase1-zbrun`.
- **Deferred follow-up (pre-existing, minor).** `Process::unregister_thread` releases the
  processor id before the real thread's `GuestThread`/JIT is destroyed. Task 5 fixed this
  ordering for borrowers only.

## Launcher bug: plugin resources not found (fixed 2026-09-15, device retest pending)

- **repostzap.** `Resources$NotFoundException` for a string that exists in its APK. Its
  Compose code localizes through `createConfigurationContext`, which returned a context
  with the launcher's resources.
- **avtobuy (Flutter).** No asset loads (`assets/cities.bin`, icon font). The Flutter engine
  takes its AssetManager from `createPackageContext(getPackageName())`.
- **Fix.** `PluginContext` wraps every derived context (configuration, display, window,
  attribution, `createContext`, device-protected storage) and package contexts for its own
  package in a `PluginContext` with plugin resources.

## Phase 4c done

Record: `docs/superpowers/plans/2026-09-15-phase4c-guest-jnienv.md`. Prototyped and verified in
`.worktrees/proto-4c`, then committed along these boundaries (each commit builds and passes
the host suite):

| Task | Commit |
|---|---|
| 4c-1.1 JNI protocol and `tools/gen_jni.py` generated tables | `2fb99a9` |
| 4c-1.2 `JniBackend` interface and mock JVM | `e9d2731` |
| 4c-1.3 `HostJni` core, object host calls, guest `libzbjni.so`, `jni_bridge_test` | `81887b9` |
| 4c-1.4 `Call*Method` and field host calls | `dbc6c2e` |
| 4c-1.5 string, array, direct buffer host calls | `608a21d` |
| 4c-2.1 `NativeSlots::release` | `48a774f` |
| 4c-2.2 Java -> guest dispatcher and `RegisterNatives` | `6b607b2` |
| 4c-2.3 `GetEnv`, `AttachCurrentThread`, `DetachCurrentThread` | `c567611` |
| 4c-2.4 `JniEnvBackend` over the real `JNIEnv` (compile-only) | `43ec3e2` |

## HANDOFF 2026-09-15 (after Phase 4c)

- **Phase 4c is committed** on `phase1-zbrun` (`2fb99a9`..`30c811c`, 10 commits). Last verified
  results:
  - host tests 18/18;
  - guest tests 9/9, including Orange Roulette `or_dlopen_dynamic`;
  - `jni_bridge_test` passed 20 of 20 repeated runs;
  - `tools/gen_jni.py --check` passes;
  - the Android build links.
- **Code review of 4c:** a Sonnet review of `2141dc6..30c811c` was running when this note was
  written. If its findings are not recorded below, run a new review before building 4d on top.
  Focus on:
  - the dispatcher thread choice and `thread_local` cleanup at thread exit;
  - `JniCall` argument capture and guest pointer bounds;
  - integer overflow in buffer sizes;
  - `NativeSlots::release`;
  - JNI misuse in the real backend (`core/android/jni_env_backend.*`, compile-only).
- **Prototype worktree:** `.worktrees/proto-4c` is detached, and its contents are already
  committed. It can be removed.
- **Next: plan 4d (Android integration).** Inputs gathered:
  - The arm32 sysroot is 10 files / 4 MB. `zbhost`, `libzbjni.so` and `libzbcompat.so` are under
    50 KB. Bundle all of them in the launcher APK; no download step is needed.
  - Port `tools/fix_guest_lib.py` (120 lines) to C++ in `core`, so the launcher (through JNI),
    zbrun and the tests share it. It handles absolute `DT_NEEDED` -> basename and `DT_TEXTREL`
    -> the `DT_ZB_TEXTREL` marker.
  - Orange Roulette libs:
    - TEXTREL in `libApplicationMain`, `liblime`, `libopenal`;
    - absolute `DT_NEEDED` in `libApplicationMain`, `liblime`;
    - `libregexp`, `libstd`, `libzlib` need no fixups.
  - 4d scope (spec part 1, section 1):
    - `libzbproxy.so`;
    - `ZBridge.onProxyLoaded`;
    - binding `Java_*` exports through the real `RegisterNatives`;
    - guest `JNI_OnLoad`;
    - launcher class loader: delegate `com.zettabridge.core.*` to the launcher, and
      `findLibrary` returns proxies for armeabi libs;
    - extracting and fixing armeabi libs at import;
    - device test T7 and the Orange Roulette smoke test.
- **Open risks from 4c:**
  - carrier cost: 2 processor ids + ~34 MiB JIT per Java thread calling natives, so a pool may be
    needed;
  - `thread_local` destructor ordering against ART detach, untested on device;
  - buffers are always copied.

## HANDOFF addendum (Claude at 90% limit)

- The Sonnet review of Phase 4c may not finish before the limit. Codex: run your own code review of `2141dc6..30c811c` first, using the focus list above, and fix any Critical or Important findings before plan 4d.
- **The `.worktrees/proto-4c` working tree differs from the committed 4c code.** Diff stat vs `30c811c` over `core guest tests tools`:  24 files changed, 6480 deletions(-).
  - Treat `phase1-zbrun` as the source of truth.
  - Before deleting the worktree, check `git -C .worktrees/proto-4c diff 30c811c` for any fix that is not yet committed. Port such fixes on purpose.
- **Next after the review: write plan 4d** (inputs above). Use the lean style of the 4c record: task list, decisions, tests, acceptance. Do not reproduce full code per task. Implement it task by task with a commit per task.

## Phase 4c review result (Sonnet, reviewed committed 30c811c) - fix these first, then plan 4d

**Verdict:** With fixes. Everything else matches the amended spec, and all earlier review decisions hold.

1. **[Important] Reused native slot is published without synchronization.**
   - Where: `core/src/jni/native_thunks.cpp`.
   - `NativeSlots::allocate` writes `targets_[reused]` under the mutex but never touches `count_`, which is what lock-free `target()` relies on.
   - Fix: add a per-slot atomic ready/generation flag. `allocate` stores it after the write; `target()` loads it before reading.
2. **[Important] 32-bit size overflow in the guest buffer allocator.**
   - Where: `guest/zbjni/zbjni.c` `zbjni_buffer_new`.
   - `((size_t)length + 1) * size` wraps on arm32 for J/D arrays (length around 2^29). The host is then told a small allocation is several GB.
   - Fix: compute the size in 64-bit and return NULL if it does not fit.
3. **[Important] `NewDirectByteBuffer` capacity is not checked against `INT32_MAX`.**
   - Where: `core/src/jni/host_jni_data.cpp`, and `core/android/jni_env_backend.cpp` passes it through unchanged.
   - Fix: validate `[0, INT32_MAX]` in `host_jni_data.cpp` before calling the backend.
4. **[Minor] Leaked guest env.** `JniThread` destructor (`core/src/jni/host_jni.cpp`) never frees the guest env of a guest pthread that attached without detaching.
5. **[Minor] Misleading abort message.** `dispatch_native` (`core/src/jni/host_jni_natives.cpp`) says "has no target" when HostJni itself is missing.
6. **[Housekeeping] Delete the prototype worktree.** `.worktrees/proto-4c` is stale, superseded prototype code. Delete it with `git worktree remove --force .worktrees/proto-4c` (it has nothing uncommitted of value).

After fixing items 1-3 (with tests), run the full suite, `tools/gen_jni.py --check` and the Android link, then write plan 4d.

## Phase 4c review fixes done (2026-09-15)

Implemented on `codex/phase4c-review-fixes` with an observed RED test before each
production change:

| Finding | Commit | Result |
|---|---|---|
| Reused native-slot publication | `9647ea8` | per-slot release/acquire ready flag; released slots are hidden until republished |
| arm32 JNI buffer size overflow | `007f582` | 64-bit checked total; oversized copied arrays return `NULL` |
| direct-buffer capacity bound | `76e00be` | values outside `[0, INT32_MAX]` fail before backend dispatch |
| Recycled mock thread ids (found by stress verification) | `67285cb` | mock JNIEnv ownership uses a unique thread-lifetime token |

The stale `.worktrees/proto-4c` worktree was compared file-by-file with `30c811c`;
all project files matched, so it was removed as instructed.

Fresh verification after all fixes:

```text
tools/build_guest.sh                         PASS
ctest --test-dir build/host                  18/18 PASS
jni_bridge_test, 20-run loop                 20/20 PASS
tools/run_guest_tests.sh                     all 9 PASS, including Orange Roulette
tools/gen_jni.py --check                     PASS
Android arm64 zbridge + zbrun                link OK
```

Still open, non-blocking Phase 4c review minors:

- a guest pthread that attaches and exits without `DetachCurrentThread` leaks its guest env;
- `dispatch_native` says "has no target" when the process-wide `HostJni` pointer is missing.

Next: write the Phase 4d plan, then implement proxy loading, ART binding, T7 and the
Orange Roulette smoke launch.

## Phase 4d Task 3 done (2026-09-15)

- Task 3 (real ART discovery backend): `JniEnvBackend::find_declared_natives` loads through the
  retained plugin loader, enumerates `getDeclaredMethods`, and builds exact descriptors with
  `zb/jni_descriptor.h`, which is tested on the host by `jni_descriptor_test`.
  - Seam `zbjni_reflection_compile_test` links with `--no-undefined`; `zbridge` links in the
    Android build.
  - `ReflectionSmoke.java` compiles against android-36.
  - Real ART behavior is checked in Task 7.
- Task 4 (standalone arm64 proxy): `core/android/zbproxy.c` -> `libzbproxy.so` (5.8 KB, NEEDED
  liblog/libdl/libc, exports only `JNI_OnLoad`) calls `static int ZBridge.onProxyLoaded(String)`;
  0 means `JNI_VERSION_1_6`, 1.2/1.4/1.6 pass, anything else or an exception gives `JNI_ERR`.
  - `zbproxy_fake_jni_test` drives it on the host; `tools/check_zbproxy.py` runs after every
    Android link and as `zbproxy_structure_test` (skipped when not built).
  - ART's `JVM_NativeLoad` clears the pending exception, so Task 5 must record failure detail itself.

## Phase 4d Task 5 done (2026-09-15)

- Portable state machine `zb::ProxyRuntime` + real graph `zb::GuestJniEngine` (LibraryRuntime,
  HostJni chained before start, JniLoader) in `core/include/zb/proxy_runtime.h`; Android glue
  `core/android/guest_jni_runtime.*` (JniEnvBackend, never destroyed) and `zbridge_jni.cpp`.
- Java API (`com.zettabridge.core.ZBridge`, launcher copy): `activatePlugin(String pluginRoot, int
  targetSdk, ClassLoader)` throws IllegalStateException; `onProxyLoaded(String)`; `loadError(String)`
  and `lastLoadError()` return the stored message or null; `fixGuestLibrary(String)` returns
  `unchanged` / `changed: ...` / `skipped: ...` or throws IOException.
- Decisions:
  - Class loader comes from an explicit `activatePlugin` call before plugin code runs.
  - Memoization is keyed by the realpath of the proxy; successes and failures are both final.
  - One plugin per process; a failed load or start needs a new `:guest` process (ART never reruns
    `JNI_OnLoad` for a path).
  - zbhost runs as `<targetSdk> libzbjni.so` with only
    `LD_LIBRARY_PATH=<files>/zb/guest/lib:<files>/plugins/<pkg>/lib`.
- Layout: `<files>/zb/{sysroot,guest/zbhost,guest/lib}`, `<files>/plugins/<pkg>/{lib,proxy}`.
- Tests: `proxy_runtime_test` (fake engine, all eight cases) and `guest_jni_engine_test_{load,
  preload-failure}` (real guest over MockJvm). Host 27/27, guest 9/9, Android links.

## Repository (2026-09-15)

- Private GitHub repo: https://github.com/ZailoxTT/ZettaBridge (GPL-3.0, README.md plus
  README.ru.md).
- Local `phase1-zbrun` tracks `origin/main`.
- Do not push without the user's agreement.
- Never commit `cc`, `cod`, screenshots or APKs; they are in `.gitignore`.
- Before the repo goes public, check `CLAUDE.md`/`AGENTS.md` for anything private.

## HANDOFF 2026-09-15 late (Claude at 85% limit)

- **Task 5** (Android guest runtime) was being implemented by a Claude Opus agent.
  - If there is no `android: connect proxy loads to the guest JNI runtime` commit, inspect the uncommitted changes and the plan's Task 5 checkboxes before continuing.
  - Keep its risk list: ART clears the `JNI_OnLoad` exception, so preserve the detailed error yourself; a failed path cannot be retried; canonicalize paths; `set_class_loader` before loads; reject a second plugin.
- **Review of Tasks 3-4** (`fcbd609`, `70989eb`, `299dd7f`) was running.
  - If its findings are not recorded here, review again. Main open question: `getDeclaredMethods` resolves the types of every method, so one missing type fails a whole library load. Consider a fallback.
- **Direction changed (user decision):** 3D games and performance work are goals after 2D (see CLAUDE.md Non-goals). The user will provide the Portal (NVIDIA Shield) APK as a future 3D target; it may need Tegra-specific GLES extensions.
- **Remote:** `origin/main` = pushed `phase1-zbrun`. Push only with the user's agreement.

## NEXT (after Task 6, 2026-09-15)

- **Done:**
  - Task 5 (`f90638b`); the Java API for Task 6 is in `android/launcher/.../core/ZBridge.java` (`activatePlugin`, `onProxyLoaded`, `loadError`, `lastLoadError`, `fixGuestLibrary`);
  - Task 6 on local branch `codex/phase4d-launcher`: launcher arm32 import, runtime bundle,
    `PluginClassLoader`, activation before plugin code, proxy routing, and persistent diagnostics;
  - host tests 27/27 and guest tests 9/9.
- **Review of Tasks 3-4:** `docs/superpowers/reviews/2026-09-15-phase4d-tasks3-4-review.md` (`61c2361`). Fix its two Important items before Task 8:
  1. `getDeclaredMethods` can fail a whole library: bind long-form exports through `GetMethodID`, and skip-and-log short-form ones.
  2. Add executable host tests for `jni_env_backend.cpp` with a fake reflective `JNIEnv`.
- **Task 5 review passed:**
  `docs/superpowers/reviews/2026-09-15-phase4d-task5-review.md`. No Critical or Important
  findings. Focused tests passed 3/3 and `proxy_runtime_test` passed 50/50 repeated runs.
- **Task 6 is complete.** Key details:
  - ABI priority is arm64-v8a, armeabi-v7a, then armeabi; old 32-bit imports require reimport;
  - staging import runs `fixGuestLibrary` before metadata/publication, removes stale native files,
    and preserves `plugins/<pkg>/data` on reimport;
  - `PluginClassLoader` delegates only `com.zettabridge.core.*`, returns real arm64 libraries or
    atomically-created `plugins/<pkg>/proxy/lib<name>.so` copies for arm32, and returns null for the
    normal system fallback when a library is absent;
  - `tools/make_launcher_bundle.sh` produces an ignored 6.9 MiB `build/launcher` tree from the exact
    10-file sysroot/runtime set. Gradle consumes only that generated assets/jniLibs tree;
  - runtime assets install atomically before plugin code and `ZBridge.activatePlugin` runs before
    providers or `Application`; a bridge load failure is shown through `Diagnostics` and the
    unusable `:guest` process exits after preserving the error.
- **Task 6 verification:** launcher contract test PASS; full Java compile against android-36 PASS;
  bundle filename/ELF/size validation PASS; Android `zbridge` and `zbproxy` link; host 27/27; guest
  9/9; `tools/gen_jni.py --check` PASS. No Gradle wrapper or system Gradle is available here, so an
  APK build was not run locally.
- **Any load failure needs a `:guest` process restart.**
- **Next:** Task 7, the real-ART T7 diagnostics app and OnePlus 13 device run. Then fix the two
  Tasks 3-4 review findings above before Task 8 (Orange Roulette smoke launch).

## Codex continuation (2026-09-15)

- Work continues on local branch `codex/phase4d-launcher`, based on `3085daa` from
  `phase1-zbrun`. Do not push without the user's agreement.
- Task 5 review and Task 6 are complete. Next is Phase 4d Task 7.
- Commit every completed task locally and update this file in the same task commit so a fresh
  Claude or Codex session can resume from the latest `NEXT` section.

## Phase 4d Task 7 done (2026-09-16)

- Task 7 is complete in this commit:
  - `guest/testlib/zbt7probe.c` and its `tools/build_guest.sh` target;
  - `android/t7/java/` real-ART model/runner and minimal plugin loader activity;
  - `android/t7/project/`, `tools/make_t7_bundle.sh`, and `docs/phase4-device-test.md`.
- `libzbt7probe.so` uses the safe portions of the existing JNI probe plus real-ART string, array,
  reference and direct-buffer checks. Its guest `JNI_OnLoad` performs `RegisterNatives`; Java then
  tests nested calls and two concurrent callers. Deliberate invalid-JNI cases remain host-only so
  CheckJNI cannot abort the diagnostics app.
- Fresh local verification with the Task 7 work:
  - `tools/make_t7_bundle.sh`: PASS, 7.0 MiB; Java compile and ELF/export checks pass;
  - Gradle 8.11.1 / AGP 8.7.3 `:app:assembleDebug`: PASS on the arm64 phone host;
    the ready APK is copied to
    `/sdcard/AndroidIDEProjects/ZettaBridge/ZBridgeT7-debug.apk` (6.0 MiB);
  - host suite 28/28; guest suite 9/9; JNI generator check PASS;
  - Android `zbridge`/`zbproxy` link and proxy structure check PASS.
- First device run reached the `strings` checkpoint and failed only at `zbt7probe.c:94`:
  current ART deliberately emits a supplementary code point as four-byte UTF-8 from
  `GetStringUTFRegion`, rather than the six-byte Modified UTF-8 form required by the JNI spec.
  The test expectation was corrected to ART behavior and the replacement APK (SHA-256
  `bc69597e7e35cf6ae56051b946c7af896a508a906cbe7196eb691fca0b592165`) was rebuilt; the next
  device run passed this checkpoint.
- Second device run passed strings and reached `RegisterNatives`, then `zb.Natives.add` was
  unresolved. Root cause: guest `FindClass("zb/Natives")` used ART's caller loader and registered
  the default app-loader copy, while T7 invoked the plugin-loader copy. `JniEnvBackend::find_class`
  now routes ordinary internal class names through the retained plugin loader. A new executable
  fake-JNI host test observed RED before the fix and PASS after it; it also made the Android-only
  backend compile and run on the host. Full verification is host 28/28, guest 9/9, generator PASS,
  Android link PASS and APK build PASS. Device rerun of APK SHA-256
  `de344ba365acf12eee9e740ef6fb1ce71de1bc5cc182c7093ea516af2677612d` passed T7 on the OnePlus 13.
- **Device acceptance:** `T7 PASS` on 2026-09-16. This covers the real ART backend, all JNI value
  types and call forms, refs, strings, arrays, direct buffers, exceptions, guest `JNI_OnLoad`,
  `RegisterNatives`, nested calls, JavaVM attach/detach and two concurrent Java callers.
- **NEXT:** finish the remaining Important Tasks 3-4 review item before Task 8: long-form native
  exports must bind without `getDeclaredMethods`, while a reflection failure for a short-form
  export must skip-and-log instead of aborting the library. Expand `jni_env_backend_test` with the
  review's missing/failure cases. Then run Task 8, the Orange Roulette Phase 4 smoke launch. Phase
  4 is complete when it reaches the first intentionally unimplemented GLES or `AAsset*` call with
  no JNI error; GLES passthrough and the first rendered frame are Phase 5.

## Phase 4d Task 8 launcher gate ready (2026-09-16)

- The first real AGP launcher build exposed `ClassLoader.getClassLoadingLock`, which exists in the
  desktop JDK used by the compile harness but not in Android's API. `PluginClassLoader` now
  synchronizes on itself, matching the T7 loader. `LauncherContractsTest` and the signed launcher
  APK build both pass.
- Ready device files:
  - launcher APK: `/sdcard/AndroidIDEProjects/ZettaBridge/ZettaBridge-launcher-debug.apk`, SHA-256
    `e6ec3a52a64dbfb556fcbf405c480c74a1b95b8fbe396914676f7fcb1c7977a3`;
  - untouched Orange Roulette APK: `/sdcard/AndroidIDEProjects/ZettaBridge/orange-roulette-1-0-0.apk`,
    SHA-256 `1fb252e27c06bc8f1a438a8bbed69f8feb75de4245a6105c04d4ca06982b3864`.
- **NEXT/device action:** install/update the launcher, import the Orange Roulette APK, launch it,
  and preserve the first on-screen/clipboard/file failure. A failed guest load requires force-stop
  of the launcher before retrying. The theoretical `getDeclaredMethods` hardening from the review
  remains open; use the real smoke result to decide whether it blocks this APK before expanding it.

## Phase 4d Task 8 first device result (2026-09-16, not complete)

- The user installed `fc10efa`'s launcher APK, imported the untouched Orange Roulette APK, and
  launched it. There was no Java/JNI error or launcher diagnostic. The plugin showed a black
  screen, briefly changed half the screen to white while switching to landscape/render setup, then
  silently returned to the launcher.
- This is the first observed end-to-end arm32 launch through the production launcher and is
  consistent with reaching the generated GLES/asset stubs. It is not yet enough to check Task 8:
  OxygenOS hides third-party logcat output, so the run did not preserve the first host-call name or
  prove the exact six proxy loads, two guest `JNI_OnLoad`s and 20 registrations.
- `Process::dispatch_stop` currently logs the first unimplemented host call, writes `r0 = 0`, and
  continues. Therefore the silent exit can happen after several zero-returning GLES calls; it is
  not necessarily the first trap itself.
- **NEXT:** expose the first non-JNI generated host call through a process-safe persistent/on-screen
  diagnostic (or begin the Phase 5 host dispatcher with equivalent tracing), rerun Orange Roulette,
  and record the exact call plus load/registration counts. Then perform the final Phase 4 regression
  and documentation commit. Do not claim Phase 4 complete from the visual symptom alone.
- The Tasks 3-4 review hardening remains open. Note that the suggested long-form-export shortcut is
  underspecified: JNI long names encode parameter types but not the return type, while
  `GetMethodID` needs the complete descriptor. A robust targeted implementation can use
  `MethodType.fromMethodDescriptorString(arguments + "V", pluginLoader).parameterArray()` followed
  by `Class.getDeclaredMethod`, then derive the actual return descriptor; short-form reflection
  resolution failures should skip-and-log. Do not blindly append a guessed return type.
