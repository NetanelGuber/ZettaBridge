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
- **Local commits only.** Pushing to GitHub is done together with the user later.

## Now: Phase 4a (JNI host units)

Plan: `docs/superpowers/plans/2026-09-14-phase4a-jni-host-units.md`. It has 6 TDD tasks
with complete code. Work through them in order. For each task, run the full host suite
(`ninja -C build/host && ctest --test-dir build/host --output-on-failure`) and make one
local commit.

**Progress (2026-09-14):**

| Task | State | Commits |
|---|---|---|
| 1. `Java_*` name decoding | done, reviewed | `5eeada8`, `96250bc` (strict ART-canonical decoding after review; the plan's Task 1 code was synced) |
| 2. Signature -> shorty | done, reviewed | `cbfe969`, `7558c25` (strict class-name scan, 255-dimension limit; plan synced), `44362e8` (docs) |
| 3. AAPCS64 -> AAPCS32 marshaling | done, reviewed | `fe12334` (both ABIs checked against disassembly from NDK arm32 clang and host clang) |
| 4. Handle tables | not started | |
| 5. Thunk pool (`thunks.S`), dispatcher, slots | not started | the assembly in the plan was prototyped and verified on this machine |
| 6. Regression run + docs | not started | |

**Open review notes to fold into later tasks:**
- **Task 3, Important. Do this first, before Task 4.**
  - An unknown shorty letter must fail loudly. In `core/src/jni/native_call.cpp`, the
    `default:` branches of `marshal_native_args` and `store_native_result` silently skip.
    In `marshal_native_args` a skipped letter also leaves the host reader unadvanced,
    which shifts every later argument.
  - Make both functions abort with a log line naming the shorty. Add a note to the
    header that shorties must come from `shorty_from_signature`.
  - Then update the plan's Task 3 code block and commit.
- **Task 3, Minor.**
  - Add tests for the host FP stack-overflow path: 9+ `F`/`D`, so `next_fp()` reads
    `regs.stack`.
  - Add tests for `(JI)V` and `(DI)V`: the long takes r2:r3, then the int goes to the
    stack.
  - Add a comment that `sign_extend32/64` rely on the implicit promotion of the
    `int8_t`/`int16_t` argument.
- **Spec wording.** Spec section 2 still describes thunks as `movz x16, #i; b ...`. The
  plan and implementation use `adr x16, .; b zb_native_common`. Fix the spec wording in
  Task 6.
- **Task 1 nits.**
  - The comment at `core/src/jni/mangle.cpp` near line 90 says "lone separator"; it
    should say "a lone '_' meaning '/'".
  - The trailing-`/` and `//` path checks are defensive only.
  - A test for `Java_pkg_Foo_bar_4x` would help.
  - One sentence noting that name parts starting with digits 0-3 cannot be decoded.
- **Task 2 nit.** The header says "malformed"; it only rejects structurally malformed
  descriptors. Characters inside class names are not validated.

## Next: plans 4b, 4c, 4d

Write each plan after 4a lands, using the real interfaces. The scope and the review
notes (per-method `RegisterNatives`, the `!` prefix, host-computed shorties, slot
release only for never-bound slots) are at the end of the 4a plan under "Following
plans". Use the same format as the 4a plan: TDD tasks with complete code.

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
