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
  reviewed. Host tests are 15/15. Next is plan 4c (generated guest `JNIEnv`, host JNI
  backend, mock JNI test).
- Work continues on branch `codex/phase4b-library-runtime` in worktree
  `.worktrees/phase4b-library-runtime`. The parent branch is
  `codex/phase4a-jni-host-units`.
- Commit locally; pushing is done together with the user (no Git remote is configured).

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
