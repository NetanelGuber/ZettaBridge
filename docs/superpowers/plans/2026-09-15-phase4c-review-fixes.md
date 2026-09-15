# Phase 4c Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three Important Phase 4c review findings before Phase 4d.

**Architecture:** Keep the existing JNI protocol unchanged. Publish reused native targets through a per-slot release/acquire flag, reject guest allocations that do not fit arm32 `size_t`, and reject direct-buffer capacities outside the signed 32-bit guest range before calling the backend.

**Tech Stack:** C++20, C11 arm32 guest code, CTest, mock JVM, real translated arm32 `libzbjni.so`.

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md` and `AGENTS.md` section "Phase 4c review result".

## Global Constraints

- Work on `codex/phase4c-review-fixes`; commit locally and do not push.
- Keep repository files English and ASCII only.
- Preserve the required unstaged five-file Dynarmic patch and unrelated untracked files.
- Generated JNI tables are changed only through `tools/gen_jni.py`; these fixes require no generated changes.
- Write and observe each regression fail for the intended reason before changing production code.

---

### Task 1: Synchronize reused native-slot publication

**Files:**
- Modify: `core/include/zb/native_thunks.h`
- Modify: `core/src/jni/native_thunks.cpp`
- Test: `tests/host/native_thunk_test.cpp`

**Interfaces:**
- Consumes: `NativeSlots::allocate`, `release`, and lock-free `target`.
- Produces: a slot-local ready state whose release store publishes `targets_[slot]` to the acquire load in `target`.

- [ ] **Step 1: Write the failing test**

After releasing slot 1, require `target(1) == nullptr`; after allocating its replacement, require the replacement target. This fails because the current upper-bound-only publication keeps the old target visible while the slot is released.

```cpp
slots.release(1);
CHECK(slots.target(1) == nullptr);
CHECK(slots.allocate({0x40000, "J", false}) == 1);
CHECK(slots.target(1) != nullptr && slots.target(1)->guest_function == 0x40000);
```

- [ ] **Step 2: Verify RED**

Run `ninja -C build/host native_thunk_test && ctest --test-dir build/host -R '^native_thunk_test$' --output-on-failure`.
Expected: the new null-target check fails.

- [ ] **Step 3: Implement release/acquire publication**

Add `std::unique_ptr<std::atomic<bool>[]> ready_`, initialize every flag false, store false in `release`, write the replacement target under the existing mutex, then store true with `memory_order_release`. Make `target` check both the count bound and an acquire load before returning the target pointer. Publish new slots through the same ready flag before advancing `count_`.

```cpp
if (slot >= count_.load(std::memory_order_acquire) ||
    !ready_[slot].load(std::memory_order_acquire)) return nullptr;
return &targets_[slot];
```

- [ ] **Step 4: Verify GREEN and commit**

Run the targeted test and `git diff --check`, then commit the three files with `jni: synchronize reused native slot publication`.

### Task 2: Reject arm32 guest-buffer size overflow

**Files:**
- Modify: `tests/host/mock_jvm.h`
- Modify: `tests/host/mock_jvm.cpp`
- Modify: `tests/host/jni_bridge_test.cpp`
- Modify: `guest/testlib/zbjniprobe.c`
- Modify: `guest/zbjni/zbjni.c`

**Interfaces:**
- Consumes: the real guest `GetLongArrayElements` path and mock array length.
- Produces: `zbjni_buffer_new` returns `NULL` whenever header plus payload cannot fit guest `size_t`.

- [ ] **Step 1: Write the failing end-to-end test**

Add a mock-only sparse primitive array constructor that records type and length without allocating backing bytes. Add a guest probe that calls `GetLongArrayElements` for an `INT32_MAX`-length long array and succeeds only when it gets `NULL`. Run it in a fresh child bridge and require exit 0, so the current wrapped allocation/follow-up region call fails the child without endangering the main test process.

```c
JNIEXPORT jint JNICALL zbjniprobe_buffer_overflow(JNIEnv* env, jobject array) {
    return (*env)->GetLongArrayElements(env, (jlongArray)array, NULL) == NULL ? 0 : __LINE__;
}
```

- [ ] **Step 2: Verify RED**

Run `tools/build_guest.sh && ninja -C build/host jni_bridge_test && ctest --test-dir build/host -R '^jni_bridge_test$' --output-on-failure`.
Expected: the overflow child exits through the existing host buffer validation/fatal path instead of 0.

- [ ] **Step 3: Implement checked 64-bit sizing**

Compute payload and total allocation sizes in `uint64_t`; reject totals above `SIZE_MAX` before converting to `size_t`. Use the checked payload size for `malloc` and `memset`.

```c
const uint64_t payload64 = ((uint64_t)length + 1) * (uint64_t)size;
const uint64_t total64 = (uint64_t)sizeof(struct zbjni_buffer) + payload64;
if (total64 > SIZE_MAX) return NULL;
const size_t payload = (size_t)payload64;
```

- [ ] **Step 4: Verify GREEN and commit**

Run the targeted test and `git diff --check`, then commit the five files with `jni: reject oversized guest JNI buffers`.

### Task 3: Bound direct-buffer capacity before backend dispatch

**Files:**
- Modify: `tests/host/jni_bridge_test.cpp`
- Modify: `guest/testlib/zbjniprobe.c`
- Modify: `core/src/jni/host_jni_data.cpp`

**Interfaces:**
- Consumes: split 64-bit capacity arguments from `libzbjni.so`.
- Produces: `NewDirectByteBuffer` accepts only capacities in `[0, INT32_MAX]` before translating its guest address or calling `JniBackend`.

- [ ] **Step 1: Write the failing test**

Add a guest probe that calls `NewDirectByteBuffer(NULL, INT32_MAX + 1)` and a child-process test that requires the mock fatal exit. The current implementation reaches the backend instead, so the child does not produce the required fatal status.

```c
JNIEXPORT jint JNICALL zbjniprobe_bad_direct_capacity(JNIEnv* env, jobject unused) {
    (void)(*env)->NewDirectByteBuffer(env, NULL, (jlong)INT32_MAX + 1);
    return __LINE__;
}
```

- [ ] **Step 2: Verify RED**

Run the targeted guest build and `jni_bridge_test`; expect the child-status assertion to fail.

- [ ] **Step 3: Add the host-side bound**

Before guest-range arithmetic or the backend call, abort through `fatal` unless `capacity >= 0 && capacity <= INT32_MAX`.

```cpp
if (capacity < 0 || capacity > INT32_MAX) {
    fatal(env, "NewDirectByteBuffer: capacity %lld is outside the 32-bit guest range",
          static_cast<long long>(capacity));
}
```

- [ ] **Step 4: Verify GREEN and commit**

Run the targeted test and `git diff --check`, then commit the three files with `jni: validate direct buffer capacity`.

### Task 4: Full regression and handoff

**Files:**
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: a clean Phase 4c base ready for the Phase 4d plan.

- [ ] **Step 1: Run complete verification**

Run `tools/build_guest.sh`, `ninja -C build/host`, `ctest --test-dir build/host --output-on-failure`, `tools/run_guest_tests.sh`, `tools/gen_jni.py --check`, and the Android arm64 `zbridge` link command from `CLAUDE.md`. Include the Orange Roulette APK case without staging the APK.

- [ ] **Step 2: Record exact results**

Update the Phase 4c review section in `AGENTS.md` with the three fix commits and verification results. Keep the guest-env leak and misleading diagnostic as explicit minor follow-ups unless they receive their own tested fixes.

- [ ] **Step 3: Commit documentation**

Run `git diff --check` and commit only `AGENTS.md` and this plan with `docs: record Phase 4c review fixes`.

## Self-review

- The plan covers all three Important Sonnet findings and does not change the JNI wire ABI.
- Every production change follows an observed RED test.
- Task interfaces use the existing `NativeSlots`, `HostJni`, `JniBackend`, and translated guest test paths.
- The two minor findings remain visible rather than being silently claimed fixed.

## Execution record

- Task 1: `9647ea8`; RED was `target(1)` remaining non-null after release.
- Task 2: `007f582`; RED reached `GetPrimitiveArrayRegion` with a false
  17,179,869,176-byte buffer after arm32 arithmetic wrapped.
- Task 3: `76e00be`; RED let capacity `INT32_MAX + 1` reach the mock backend.
- Stress verification then exposed recycled `std::thread::id` reuse in the mock JVM.
  A focused unit test reproduced stale JNIEnv ownership; `67285cb` replaced the key
  with a unique thread-lifetime token. `jni_bridge_test` then passed 20/20.
- Final verification: host 18/18, guest 9/9 including Orange Roulette, generated JNI
  files current, and Android arm64 `zbridge` plus `zbrun` linked.
