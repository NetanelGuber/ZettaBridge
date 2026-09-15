# Phase 4c: Guest JNIEnv, Java -> Guest Native Calls, and JavaVM (Record)

> This is an implementation record, not a step-by-step plan. The code was prototyped and
> verified in the `proto-4c` worktree, then committed to `phase1-zbrun` along the task
> boundaries below. Each commit builds and passes the full host suite. Read the files
> named here for the code.

**Goal:** Give arm32 guest native code a working 32-bit `JNIEnv` / `JavaVM`, and let Java
call guest native methods. Everything runs on this machine against a mock JVM, with no ART.

**Architecture:**
- **Guest side.** `libzbjni.so` (arm32 C, preloaded by `zbhost`) holds the full
  `JNINativeInterface` and `JNIInvokeInterface` tables. Almost every slot forwards to one
  flat host call (`svc #0x5AFCxx`).
- **Host side.** `HostJni` (`core/src/jni/host_jni*.cpp`) serves those calls against the
  abstract `JniBackend`:
  - it translates 32-bit handles, ids and guest memory;
  - it runs Java -> guest native calls through the Phase 4a thunk pool;
  - each Java thread uses its own guest thread: the one it already runs, or a cached
    carrier lease (Phase 4b).
- **Backends.** `MockJvm` (host tests) and `JniEnvBackend` (Android, the real `JNIEnv`).

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md` (sections 2-4),
amended in the same change (see "Spec amendments").

## What was verified

In `.worktrees/proto-4c` (2026-09-15):

- **Bridge test.** `jni_bridge_test` passes; it drives the probe library through the real
  arm32 linker and bionic and `libzbjni.so`. Repeated runs: 30/30 (before the restructure)
  and 25/25 (final layout).
- **Host suite and Android link.** All 18 host tests pass, and `ninja -C build/android-arm64
  zbridge zbrun` links with `core/android/jni_env_backend.cpp`.
- **Task boundaries.**
  - Every boundary below was built and tested on its own: host build, guest build, and
    `gen_jni_check`, `mock_jvm_test`, `native_thunk_test` and `jni_bridge_test` at each
    stage.
  - Reversing all stages reproduced git HEAD exactly for the 31 touched files.
- **Main checkout.** Each commit on `phase1-zbrun` was then built and passed the full
  `ctest` suite.

Final verification in the main checkout is in "Acceptance".

## Tasks and commits

### 4c-1: Guest-called JNI

| Task | Commit | Files | Covered by |
|---|---|---|---|
| 1. Protocol and generator | `2fb99a9` | `tools/gen_jni.py`, `core/include/zb/jni_protocol.h`, generated `core/include/zb/jni_hostcalls.h`, `core/src/gen/jni_hostcalls.inc`, `guest/zbjni/gen/{tables.inc,hostcalls.S,hostcalls.h}`; `tools/gen_stubs.py` reserves 0xFB00+ | `gen_jni_check` (runs `gen_jni.py --check`, including a self-test of the host-stub path) |
| 2. Backend interface and mock JVM | `e9d2731` | `core/include/zb/jni_backend.h`, `tests/host/mock_jvm.{h,cpp}`, `tests/host/mock_jvm_test.cpp` | `mock_jvm_test` |
| 3. HostJni core, objects group, guest `libzbjni.so` | `81887b9` | `core/include/zb/host_jni.h`, `core/src/jni/host_jni_internal.h`, `core/src/jni/host_jni.cpp`, `core/src/jni/host_jni_objects.cpp`, `guest/zbjni/zbjni.c`, `guest/testlib/zbjniprobe.c`, `tests/host/jni_bridge_test.cpp`; `tools/build_guest.sh` | `jni_bridge_test`: `check_invalid_handle`, `check_objects` |
| 4. Calls and fields | `dbc6c2e` | `core/src/jni/host_jni_values.cpp` | `check_values` |
| 5. Strings, arrays, direct buffers | `608a21d` | `core/src/jni/host_jni_data.cpp` | `check_data` |

### 4c-2: Java -> guest, RegisterNatives, JavaVM, ART backend

| Task | Commit | Files | Covered by |
|---|---|---|---|
| 1. Slot release | `48a774f` | `core/include/zb/native_thunks.h`, `core/src/jni/native_thunks.cpp` | `native_thunk_test` |
| 2. Dispatcher and RegisterNatives | `6b607b2` | `core/src/jni/host_jni_natives.cpp`; `HostJni::register_native` | `check_natives` |
| 3. JavaVM | `c567611` | `core/src/jni/host_jni_vm.cpp` | `check_vm` |
| 4. ART backend (compile-only) | `43ec3e2` | `core/android/jni_env_backend.{h,cpp}` in the `zbridge` target | Android link |

Docs and spec amendments: the last commit.

### What each task does

**1. Protocol and generator.**
- **Index ranges.** `jni_protocol.h` fixes the host-call ranges:
  - 0xFC00-0xFCFF: flat JNI calls, indices generated in list order;
  - 0xFB00-0xFBFF: `JNINativeInterface` slots served by host stubs, at 0xFB00 + slot.
- **Shared layouts.** It also defines the call kinds `ZB_JNI_CALL_VIRTUAL`, `NONVIRTUAL`,
  `STATIC` and `NEW_OBJECT`, the `zb_jni_guest_api` handshake, and the guest
  `JNINativeMethod` / `JavaVMAttachArgs` layouts.
- **Generator.** `gen_jni.py` reads the NDK `jni.h` (229 + 5 functions, with reserved slots
  counted) and writes:
  - the host-call indices;
  - the ARM `svc` stubs;
  - both interface tables as designated initializers.
- **Host-stub list.** `HOST_SLOTS` (the slots moved to host stubs) is empty.
- **Committed outputs.** The generated files are committed; `--check` fails when they are
  stale.

**2. JniBackend and mock.**
- **Interface.** `JniBackend` is the Java side as one virtual function per JNI operation.
  It uses opaque `uint64_t` env, refs and ids, so core never includes `jni.h`.
- **Mock model.** `MockJvm` models classes, virtual dispatch, fields, strings (modified
  UTF-8), arrays, ART-like local frames, global and weak references (with `collect()`),
  exceptions, monitors, native registration, reflection, direct buffers and attached
  threads.
- **Discipline checks.** It records JNI discipline violations in `errors()`:
  - a wrong-thread env;
  - stale or deleted references;
  - calls with a pending exception;
  - deleting a native-method argument reference.

**3. HostJni core, objects, `libzbjni.so`.**
- **Entry point.** `HostJni::handle_host_call` is chained by the owner into
  `LibraryRuntime::set_host_call_handler`. The `Register` host call from the `libzbjni.so`
  constructor validates and stores the guest API.
- **Per-thread state.** A `JniThread` (C++ `thread_local`) holds the host env, the guest
  `JNIEnv*`, the `LocalHandles`, the guest `PushLocalFrame` depth and the carrier lease.
- **`call_native(env, return_type, function, build)`** runs guest code as a native method
  of the calling Java thread.
- **Objects group.** Classes, member ids (the shorty is written back for method ids),
  reflection, objects, references, local frames, monitors and exceptions.
- **Guest table.** `zbjni.c` implements all 229 slots:
  - `...` and `va_list` are converted to `jvalue[]` by the shorty;
  - the shorty cache;
  - buffers with a header and release modes;
  - the `JavaVM` table.

**4. Calls and fields.**
- **One call for all forms.** `CallMethodA(kind, type, obj, cls, method, args)` serves
  all 93 `Call*` variants and the three `NewObject*`.
- **Argument decoding.** The host reads `count` 8-byte guest `jvalue`s, decoding each by
  the method shorty; references are resolved.
- **Fields.** `GetField(is_static, type, obj, field)` and
  `SetField(is_static, type, obj, field, low, high)`.

**5. Strings, arrays, direct buffers.**
- **Strings and arrays.** Region host calls write straight into guest memory. The guest
  builds `Get*Elements` / `GetString*Chars` / `Get*Critical` copies on top of them.
- **Direct buffers.** `NewDirectByteBuffer` maps a guest address to `base + addr`.
  `GetDirectBufferAddress` translates back, or returns NULL (logged once) for memory
  outside the reservation.

**4c-2 / 1. Slot release.** `NativeSlots::release(slot)` returns a slot for reuse, and
`allocate` hands released slots out first. Only a slot whose backend registration failed
may be released: Java never reached it, so the lock-free `target()` stays safe.
Releasing a slot that was never allocated aborts.

**4c-2 / 2. Dispatcher and RegisterNatives.**
- **Installation.** The `HostJni` constructor installs the process-wide dispatcher; a
  second instance aborts.
- **Dispatch.** The dispatcher looks up the slot and calls
  `call_native(x0, shorty[0], target, marshal_native_args)`, then `store_native_result`.
- **`register_native`** (public, for 4d's `Java_*` binding):
  1. strips one leading `!`;
  2. computes the shorty;
  3. allocates a slot;
  4. calls the backend once;
  5. releases the slot if the call fails.
- **Guest `RegisterNatives`** calls it per method and stops at the first failure with
  `JNI_ERR`.

**4c-2 / 3. JavaVM.**
- **`GetEnv`** returns this thread's guest env while it has a host env.
- **`AttachCurrentThread(AsDaemon)`.** On a thread without an env it attaches through the
  backend (name and group taken from the args), allocates the guest env and writes it out.
  On a thread that already has one, it returns that env.
- **`DetachCurrentThread`:**
  - it returns `JNI_ERR` unless the thread was attached through the guest and has no Java
    frames;
  - otherwise it detaches in the backend, resets locals and frees the guest env.

**4c-2 / 4. ART backend.** `JniEnvBackend(JavaVM*)` maps each operation to one `JNIEnv`
call. The non-trivial parts:
- **`from_reflected_method`** rebuilds a descriptor through reflection
  (`getParameterTypes` / `getReturnType`, references erased to `Object`, constructors
  return `V`).
- **`get_array_element_type`** uses `Class.getName()`.
- **Odd-aligned `jchar` buffers** are copied.
- **`GetStringUTFRegion`** uses a zeroed 3n+1 buffer.

It is compiled and linked only; it first runs in T7 (4d).

## Design decisions

1. **Frames around native calls use the backend.**
   - `call_native` brackets the guest call with `backend.push_local_frame(env, 16)` /
     `pop_local_frame`, and pushes a matching `LocalHandles` frame.
   - Host locals created during the call are freed in one step. An `L` result is handed
     through `PopLocalFrame(result)`, so it survives into the caller's frame.
   - Argument references are never deleted. ART logs "failed to find entry" and dumps the
     Java stack when native code deletes a JNI transition reference, and the mock records
     that as an error.
   - A guest native that returns with unpopped `PushLocalFrame` frames is logged, and the
     frames are popped.
2. **`JniCall` captures r0-r3 before clearing r0/r1.** The first prototype zeroed `r0`/`r1`
   (the default result) before the serve functions read their arguments, so every first
   argument read as 0. Found by the invalid-handle probe.
3. **`fatal()` skips the backend when env == 0.**
   - Invalid handles, ids, guest memory and call types log, then call backend
     `FatalError`, then abort.
   - Without a host env (before attach), `FatalError` cannot be called safely, so it logs
     and aborts.
4. **The dispatcher cannot report guest failures to Java.**
   - `call_native` returns nullopt only when the frame could not be opened (a Java exception
     is already pending) or the guest call failed.
   - The dispatcher returns zero in the first case. In the second case it calls `fatal()`.
5. **Guest thread choice.** `invoke()` uses `call_on_current` if the host thread already
   runs guest code; otherwise it uses a carrier borrowed once and kept in `JniThread`.
   - **Thread exit.** The `thread_local` destructor frees the guest env on that carrier,
     then ends the lease.
   - **Guest pthreads** free their env in `DetachCurrentThread`.
6. **Shorties.**
   - **Host.** It computes the shorty from the signature the backend accepted, keeps it per
     guest method id, and checks each call's result type against it (CheckJNI-like; for
     `NEW_OBJECT` the method must return `V`).
   - **Guest cache.** A lock-free two-level table covering 4,194,304 dense ids.
     `GetMethodShorty` is the fallback past that range, and the probe exercises it with a
     raw `svc`.
7. **Buffers.**
   - Each buffer has a 16-byte header: magic, length and type. One element of slack
     leaves room for the terminator.
   - `isCopy` is always `JNI_TRUE`.
   - A release with a pointer that no Get call returned is a `FatalError`.
   - `GetStringUTFRegion` writes a NUL terminator, as ART does.
8. **Handles.**
   - Local handles are per thread, with one frame per native call plus guest
     `PushLocalFrame` frames.
   - Globals and weaks live in separate `GlobalHandles`; method and field ids in separate
     `IdTable`s.
   - `GetObjectRefType` answers from the handle kind and validity alone.
9. **ART-compatible edges.**
   - `DefineClass` returns NULL.
   - `ExceptionDescribe` clears the exception.
   - `NewStringUTF(NULL)` and `GetStringUTFChars(NULL)` return NULL.
   - `GetEnv` with an unknown version returns `JNI_EVERSION`.
   - `DestroyJavaVM` returns `JNI_ERR`.
10. **Probe split.** Each probe uses only the host-call groups committed up to its task.
    - Objects: `objects`, `objects_after_gc`, `throw`, `bad_handle`.
    - Values: `calls`, `fields`, `exceptions`. String arguments come from Java.
    - Data: `strings`, `arrays`, `direct_buffers`.
    - Natives: `register`.
    - JavaVM: `vm`, `attach`.
11. **One HostJni per process, process-lifetime.** Its destructor aborts, like
    `LibraryRuntime`'s.

## Acceptance

On this machine:
- `tools/gen_jni.py --check`;
- `tools/build_guest.sh; ctest --test-dir build/host --output-on-failure` with all 18 tests
  passing;
- `tools/run_guest_tests.sh` with `or_dlopen_dynamic` passing;
- `jni_bridge_test` passing 20 consecutive direct runs;
- `ninja -C build/android-arm64 zbridge zbrun` linking.

`jni_bridge_test` covers:
- **Calls.**
  - Every `Call*` form (`...`, `V`, `A`) x virtual / nonvirtual / static x all 10 return
    types.
  - `NewObject` in three forms.
  - An all-types argument mix through each form.
  - A reflected method id.
  - The raw `GetMethodShorty`.
- **Fields.** Instance and static, all 9 types.
- **Strings.** UTF-8, and UTF-16 with an embedded NUL and a surrogate pair. Region calls,
  critical access, out-of-range exceptions, and NULL.
- **Arrays.** All 8 primitive types with release modes 0, `JNI_COMMIT` and `JNI_ABORT`.
  Critical arrays, object arrays, out-of-range.
- **References and frames.** Local frames and `PopLocalFrame`, local deletion, global and
  weak references including collection, and `GetObjectRefType`.
- **Exceptions.**
  - Guest `ThrowNew` pending for Java.
  - A Java -> guest -> Java (throws) -> guest (rethrows) round trip.
- **Other object calls.** Reflection, monitors, direct buffers (guest and foreign), and an
  invalid handle ending in `FatalError` (exit 86).
- **Natives.**
  - `RegisterNatives` with `!` and a failure: slot 4 is released and reused.
  - Thunk calls of `(IFFIFF)I`, `(J)V` and an `L` result.
  - Nesting Java -> guest -> Java -> guest three levels deep.
  - Two Java threads x 200 iterations on distinct carriers, with no leaked locals; the
    carriers are released when their threads exit.
- **JavaVM.** `GetEnv` versions, `AttachCurrentThread` and `DetachCurrentThread` on a Java
  thread, `DestroyJavaVM`, and `AttachCurrentThreadAsDaemon` with a name, a Java call and
  detach from a guest pthread.

## Deferred

- `ZB_JNI_STATS` and any host-stub slot implementation. `HOST_SLOTS` stays empty; the
  0xFB00 range aborts with "not implemented".
- `NativeActivity`, `System.load(path)`, and guest class loaders.
- 4d:
  - the proxy library;
  - `onProxyLoaded`;
  - `Java_*` export binding through `HostJni::register_native`;
  - guest `JNI_OnLoad` through `call_native` with `r0` = `guest_java_vm()`;
  - the launcher class loader and on-device lib fixups;
  - wiring `JniEnvBackend` into `zbridge_jni.cpp`;
  - T7 and the Orange Roulette smoke test.

## Open risks

- **`JniEnvBackend` has never run.**
  - Reflection-based `from_reflected_method` and `get_array_element_type` may trip CheckJNI
    on device.
  - ART aborts on `NewDirectByteBuffer` capacities above `INT32_MAX`.
- **Carrier cost per Java thread.** Every Java thread that calls a guest native keeps a
  lease: two Dynarmic processor ids (of 256) plus about 34 MiB of JIT. Apps whose many binder
  threads call natives could exhaust these. A carrier pool with idle release may be needed
  in 4d.
- **`thread_local` destructors on Java threads** run guest code (`free_env` and carrier
  release) at host thread exit. The ordering against ART's own thread-exit detach is
  untested on the device.
- **Cost per JNI function.** Each call is one host call: a Dynarmic halt, the handler, and
  the backend. `PushLocalFrame`/`PopLocalFrame` adds two ART calls per native call. Not yet
  measured; the performance fallback is designed but not implemented.
- **No wrong-thread `JNIEnv` detection.** The host keys state by host thread, not by the
  guest `JNIEnv*`. A guest passing its env to another thread silently uses that thread's
  state.
- **Delayed signals.** Guest signals aimed at a thread blocked in a long host call
  (`MonitorEnter` waiting in ART) are delivered only when the call returns.
- **Copy cost of buffers.** `Get*Elements` and `Get*Critical` always copy. Large audio or
  texture arrays pay a copy each frame; these are the first performance-fallback
  candidates.

## Spec amendments (same change)

In `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`:

1. **`JNIInvokeInterface` size.** It has 5 functions and 3 reserved slots (not 6 functions).
2. **Section 2, "Which thread runs the guest".** The carrier binding is kept in a host
   `thread_local` (`JniThread`), whose destructor frees the guest env and releases the
   lease. It is not a `pthread_key`.
3. **Section 2, "Frame, exceptions, crashes".**
   - The call frame is a backend `PushLocalFrame`/`PopLocalFrame` pair; an `L` result passes
     through `PopLocalFrame`.
   - Argument references are never deleted.
   - A guest call that fails without a pending exception is fatal.
4. **Section 3, host side.**
   - Host-call ranges: 0xFC00-0xFCFF flat, 0xFB00-0xFBFF slot stubs.
   - `NewObject*` goes through `CallMethodA` with kind `NEW_OBJECT`.
   - Added host calls: `GetArrayElementType`, `GetMethodShorty`.
   - The result type is checked against the shorty.
   - The host side is `HostJni` over `JniBackend`, split into group files.
5. **Section 3, guest side.**
   - The shorty cache is a lock-free two-level table covering 4,194,304 ids.
   - Buffers carry a header; a mismatched release is a `FatalError`.
   - `GetStringUTFRegion` writes a NUL terminator.
6. **Section 3, JavaVM.**
   - `GetEnv` returns `JNI_EVERSION` for unknown versions.
   - Attach on a thread that already has an env returns it.
   - `DetachCurrentThread` returns `JNI_ERR` unless the thread was attached through the
     guest and has no Java frames.
7. **Section 3, performance fallback.** The slot stubs are generated, but no host stub is
   implemented yet. `ZB_JNI_STATS` is deferred.
8. **Section 4, tests.** The mock runs in host ctest (`mock_jvm_test`, and
   `jni_bridge_test` with `libzbjni.so` in `LibraryRuntime` plus `libzbjniprobe.so`). It
   replaces the planned `jni_mock_dynamic` guest test under `zbrun`.
9. **Implementation units.** Updated with the `host_jni_*.cpp` groups, `jni_backend.h`,
   `jni_protocol.h`, `core/android/jni_env_backend.cpp`, `tests/host/mock_jvm.*`, and
   `guest/testlib/zbjniprobe.c`.
