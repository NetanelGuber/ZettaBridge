# Review: Phase 4d Task 5 Android guest runtime (2026-09-15, Codex)

- Commit reviewed: `f90638b` (`android: connect proxy loads to the guest JNI runtime`).
- **Verdict: Pass.** No Critical or Important findings.

## Checked

- `ProxyRuntime` serializes the first start, memoizes each proxy outcome, rejects a second plugin,
  and permits independent proxy loads to run concurrently.
- Guest `dlopen`, `dlsym`, `dlerror`, export binding, and `JNI_OnLoad` stay on the calling Java
  thread's cached carrier (or its current nested guest thread).
- The real Android engine retains one plugin class loader, clears and preserves pending exception
  detail before ART replaces it, and intentionally keeps the runtime graph alive for the process.
- JNI version filtering, canonical proxy paths, fixed runtime layout, preload failure, and detailed
  screen-visible errors match the approved design and the Phase 4d plan.

## Verification

```text
ninja -C build/host proxy_runtime_test guest_jni_engine_test     PASS
ctest -R 'proxy_runtime_test|guest_jni_engine_test_'             3/3 PASS
proxy_runtime_test repeated 50 times                            50/50 PASS
```

## Residual device checks

- The portable state machine and real guest engine execute on the host, but the thin JNI entry
  points and real ART exception text still require Task 7 on-device coverage.
- The existing carrier cost (two processor ids and about 34 MiB of JIT state per Java caller) is a
  performance risk, not a Task 5 correctness blocker.

