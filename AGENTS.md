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
- **Phase 4b Tasks 1-3 done and independently reviewed.** The real arm32 `zbhost`
  starts through the bionic linker and publishes its service ABI. Host tests are 13/13.
- Work continues on branch `codex/phase4b-library-runtime` in worktree
  `.worktrees/phase4b-library-runtime`. The parent branch is
  `codex/phase4a-jni-host-units`.
- The user allows pushing the dedicated Codex branch, but this checkout currently has
  no Git remote. Do not push another branch; configure/confirm the remote with the user
  first.

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
| 6. Regression run + docs | done | `docs: Phase 4a done` |

All Phase 4a review notes were folded into the implementation, tests, plan, and spec.

## Phase 4b in progress

The executable TDD plan is
`docs/superpowers/plans/2026-09-14-phase4b-library-runtime.md`. Continue at **Task 4**;
no Task 4 production or test file is currently modified.

| Task | State | Commit |
|---|---|---|
| 1. Nested host-to-guest call frame | done, reviewed | `d0708d8` |
| 2. Reusable Process stop dispatch | done, reviewed | `fcaef77` |
| 3. Fixed `zbhost` service protocol | done, reviewed | `49608ec` |
| 4. Service-thread library runtime | next | |
| 5. Carrier leases and guest-tid routing | not started | |
| 6. Regression, Android link, and docs | not started | |

Task 1 has one deferred minor review note: make the intermediate host-call register
mutation observable in `guest_call_test` and exercise handler-false after execution.
It is not blocking and can be folded into Task 6.

The Superpowers SDD scratch ledger and generated Task 1-4 briefs are under
`.superpowers/sdd/2026-09-14-phase4b-library-runtime/` in the Phase 4b worktree. The
directory is intentionally git-ignored. Task 4 was dispatched once but stopped before
any file change so Claude can resume from a clean task boundary.

The isolated worktree uses an ignored `sysroot` symlink. Its Dynarmic submodule has
the same five-file uncommitted baseline patch as the main checkout. That patch is
required: a clean recorded Dynarmic revision fails `fault_pc_test` and encounters an
unsupported `ldab` in the Android linker. Never stage the submodule pointer or modify
that patch as part of Phase 4b.

Last verified at `49608ec`:

```text
tools/build_guest.sh                         PASS
ctest --test-dir build/host                  13/13 PASS
tools/run_guest_tests.sh                     all available cases PASS
or_dlopen_dynamic                           SKIP (Orange Roulette APK absent in worktree)
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
