# Review: Phase 4d Tasks 3-4 and the GNU hash fix (2026-09-15, Sonnet)

- Commits reviewed: `fcbd609` (Task 3 ART discovery), `70989eb` (Task 4 proxy), `299dd7f` (GNU hash
  bound).
- **Verdict: With fixes.**
  - The proxy and the GNU hash fix are correct and well tested.
  - The ART backend's exception and frame discipline matches ART `check_jni.cc`:
    Push/PopLocalFrame and DeleteLocalRef are allowed while an exception is pending.

## Fix before Task 8 (Orange Roulette smoke test)

1. **[Important] `getDeclaredMethods()` can fail a whole library load.**
   - Where: `core/android/jni_env_backend.cpp` `find_declared_natives`, and its consumer
     `core/src/jni/loader.cpp` (`Error` is fatal for the library).
   - What goes wrong: `Class.getDeclaredMethods()` eagerly resolves the parameter and return types
     of every declared method. A single unresolvable type throws `NoClassDefFoundError` and aborts
     binding for every native in that `.so`.
   - Why it matters for Orange Roulette: it bundles AdMob classes next to lime, so this is a
     plausible trigger.
   - Fix, in two parts:
     - Long-form exports (`__signature` suffix): skip reflection. Bind with
       `GetMethodID`/`GetStaticMethodID`, using the decoded descriptor from `loader.cpp`.
     - Short-form exports: turn a `getDeclaredMethods()` exception into skip-and-log for that
       export (a new `NativeLookupStatus`, or the `MissingClass` path) instead of aborting the
       library.
   - **Fixed in `19bc37a`.** Long-form exports resolve their own signature through
     `MethodType.fromMethodDescriptorString(arguments + "V", pluginLoader).parameterArray()` plus
     `Class.getDeclaredMethod`, then take the return type from the found `Method` (a JNI long name
     encodes no return type, so `GetMethodID` alone cannot be used). Short-form exports still
     enumerate but report the new `NativeLookupStatus::Unresolvable`, which the loader skips, logs
     once and counts in `JniLoadReport::skipped_exports`.
2. **[Important] The real ART backend has no executable tests.**
   - Current state: `zbjni_reflection_compile_test` only links.
   - Fix: add a host test that drives `jni_env_backend.cpp` against a fake reflective `JNIEnv`,
     following the pattern of `tests/proxy/fake_jni.c`. Cover success, missing class,
     `getDeclaredMethods` throwing, an unresolvable parameter type, and PushLocalFrame failure.
   - **Fixed in `fc10efa` and `19bc37a`.** `tests/host/jni_env_backend_test.cpp` is that test; it
     now covers the whole list on both discovery paths, and again with `MethodType` unavailable.

## Minor

- **`AGENTS.md` Task 3 note is inaccurate.** It says `zbridge` links with `--no-undefined`. Only
  `zbjni_reflection_compile_test` and `zbproxy` do.
- **`299dd7f` commit message** says "about 2^40", but the test is about 2^36. Cosmetic only.
