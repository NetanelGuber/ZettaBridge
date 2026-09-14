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
- **Phase 4b Tasks 1-3 done, reviewed, and fixed after review** (`a15b86c`). Tasks 4-6
  are ready: the plan was revised after review (`203d8ac`) and its Task 4-5 code was
  prototyped, built and tested before being written down. Host tests are 13/13.
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

## Phase 4b in progress

The executable TDD plan is
`docs/superpowers/plans/2026-09-14-phase4b-library-runtime.md`. Continue at **Task 4**;
no Task 4 production or test file is currently modified. The plan's "Review decisions"
table (D1-D14) says where each review decision lands; the specs were amended to match.

| Task | State | Commit |
|---|---|---|
| 1. Nested host-to-guest call frame | done, reviewed, review fixes | `d0708d8`, `a15b86c` |
| 2. Reusable Process stop dispatch | done, reviewed, review fixes | `fcaef77`, `a15b86c` |
| 3. Fixed `zbhost` service protocol | done, reviewed | `49608ec` |
| 4. Service-thread library runtime | ready, plan revised after review | `203d8ac` (plan) |
| 5. Carrier leases and guest-tid routing | ready, plan revised after review | `203d8ac` (plan) |
| 6. Regression, Android link, and docs | ready, plan revised after review | `203d8ac` (plan) |

`a15b86c` fixed: IT/E bits cleared on call entry, `call_depth`, thread exit inside a call
ends the host process, stray or wrong-`sp` return svc is SIGILL, shared `after_stop`,
code cache size parameter, guest tid in crash reports, `_Exit` in `check.h`, and the
extended `guest_call_test`.

Phase 4a hardening after review (thunk abort/CFI/exception barrier, handle serials and LIFO
reclaim) landed on this branch before Task 4.

The Task 1 review note (observable register mutation, handler-false after execution) is
done in `a15b86c`.

The Superpowers SDD scratch ledger and generated Task 1-4 briefs are under
`.superpowers/sdd/2026-09-14-phase4b-library-runtime/` in the Phase 4b worktree. The
directory is intentionally git-ignored. Task 4 was dispatched once but stopped before
any file change so Claude can resume from a clean task boundary.

The isolated worktree uses an ignored `sysroot` symlink. Its Dynarmic submodule has
the same five-file uncommitted baseline patch as the main checkout. That patch is
required: a clean recorded Dynarmic revision fails `fault_pc_test` and encounters an
unsupported `ldab` in the Android linker. Never stage the submodule pointer or modify
that patch as part of Phase 4b.

Last verified at `203d8ac`:

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

## HANDOFF 2026-09-15 (Claude limit reached mid-task)

A Claude Opus agent was working through the 4b review fixes (decisions D1-D14 are in the Claude session and summarized here).

- **Committed:** `a15b86c`. IT state is cleared on calls; guest exit and a stray host-return svc inside a call are fatal.
- **UNCOMMITTED in this worktree (do not lose it):** a working Task 4-5 implementation used as a prototype:
  - `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp`
  - `tests/host/library_runtime_test.cpp`, `guest/testlib/zbcallprobe.c`
  - edits to `zbhost.c`, `library_protocol.h`, `signals.cpp`, `guest_thread.*`, `process.h`, `gen_stubs.py`, `build_guest.sh` and both CMakeLists
- **Last known state of that work:**
  - 14/14 host tests pass, and 50 repeated runs had 0 failures.
  - The guest suite passes and the Android build links.
  - First borrow takes about 31 ms, first call about 0.43 ms.
  - A snapshot is in the Claude scratchpad `task5/tracked.diff`.
- **Was in progress:** red-checks (temporarily break the code and confirm the test fails) for tgkill borrower-first routing and for D7 sigmask inheritance.
- **Remaining:**
  1. Finish those checks.
  2. Commit the Task 4-5 work in focused commits.
  3. Rewrite plan Tasks 4-6 to match.
  4. Amend the specs: carrier state inheritance, futex park woken by signals, exit-in-call policy, process-lifetime runtime, loader on a carrier.
  5. Run the regression suite and Android link again, then update this table.
- **Note:** ninja once warned "premature end of file"; check disk space (the disk is about 99% full).

## Phase 4b status after Claude session (2026-09-15)

- **Task 4:** `d3b7119`. Its P1 fix (retire the process signal target safely) is `5e21137`.
- **Task 5:** `1c3d145` (clear thread-local before freeing cloned threads) and `75015d9`
  (carriers). The review fix (tkill/tgkill post under the registry lock) is the next commit.
- **Verification:** 15/15 host tests, 10 of 10 repeated `library_runtime_test` runs pass, the
  guest suite passes, and the Android build links.
- **Next: Task 6** (regression, Android link, docs). Then merge the branches into
  `phase1-zbrun`.
- **Deferred follow-up (pre-existing, minor).** `Process::unregister_thread` releases the
  processor id before the real thread's `GuestThread`/JIT is destroyed. Task 5 fixed this
  ordering for borrowers only.
