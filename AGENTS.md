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
- **Local commits only.** Pushing to GitHub is done together with the user later.

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

## Next: plans 4b, 4c, 4d

Draft plan 4b is at
`docs/superpowers/plans/2026-09-14-phase4b-library-runtime.md`; review it before
implementation. It is based on the real Phase 4a interfaces.

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

## Launcher bug: plugin resources not found (fixed 2026-09-15, device retest pending)

- **repostzap.** `Resources$NotFoundException` for a string that exists in its APK. Its
  Compose code localizes through `createConfigurationContext`, which returned a context
  with the launcher's resources.
- **avtobuy (Flutter).** No asset loads (`assets/cities.bin`, icon font). The Flutter engine
  takes its AssetManager from `createPackageContext(getPackageName())`.
- **Fix.** `PluginContext` wraps every derived context (configuration, display, window,
  attribution, `createContext`, device-protected storage) and package contexts for its own
  package in a `PluginContext` with plugin resources.
