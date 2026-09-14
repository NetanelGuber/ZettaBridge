# Part 1: Java <-> native JNI bridge (Phase 4)

Status: design approved 2026-09-14. Builds on
`2026-09-13-guest-system-boundary-design.md` (part 3): guest process, host calls,
nested host->guest calls, carrier threads.

## Goal and constraints

Guest Java runs on the host's 64-bit ART. Its `native` methods live in arm32 libraries
that run translated. This part connects the two in both directions:

- **Loading.** `System.loadLibrary` on an arm32 library.
- **Calls from Java.** Java calls a guest native method.
- **Calls from guest code.** Guest code uses a 32-bit `JNIEnv` / `JavaVM`.

Constraints:
- **No root, no ART hooks.** It must work on any 64-bit-only phone and survive Android
  updates.
- **Generic.** Everything is generated from `jni.h` and method signatures. Nothing is
  specific to Orange Roulette or Haxe.
- **Performance fallback for old 3D games.** Any JNI function can move to a host-only
  implementation without touching guests (see "Performance fallback").

Measured surface of `jni.h` (NDK r29):
- `JNINativeInterface` has 229 functions:
  - 93 `Call*Method`: 31 each with `...`, `va_list` and `const jvalue*`;
  - 34 buffer get/release functions.
- `JNIInvokeInterface` has 6 functions.

Orange Roulette needs:
- 20 Java `native` methods (19 in `org.haxe.lime.Lime`, 1 in `org.haxe.HXCPP`);
- `JNI_OnLoad` in `liblime.so` and `libopenal.so`;
- callbacks into `GameActivity`, `HaxeObject`, `MainView`, `Value`, the boxed
  primitives and `AssetManager`;
- `android.media.AudioTrack` from the OpenAL mixer thread.

## 1. Loading arm32 libraries

### Interception through the plugin class loader

Guest dex is loaded by our class loader (Phase 0). It overrides `findLibrary(name)`:

| `lib<name>.so` in the APK | `findLibrary` returns |
|---|---|
| in `lib/arm64-v8a` | the real path (native, no translation) |
| in `lib/armeabi-v7a` or `lib/armeabi` | a proxy path `plugins/<pkg>/proxy/lib<name>.so` |
| not in the APK | `null`, so ART falls back to system libraries as usual |

- **A plugin never mixes ABIs.** An APK with `arm64-v8a` libs runs fully 64-bit.
- **The proxy is a copy of `libzbproxy.so`**, a tiny arm64 library, one copy per guest
  library, created on first request.
- **One file per library matters.** ART keys loaded libraries by path, so each copy loads
  separately and gets its own `JNI_OnLoad` call.

### The proxy

- **No `DT_NEEDED` on `libzbridge.so`.** The proxy links against nothing of ours.
- **Its `JNI_OnLoad(vm)`:**
  1. Finds its own path with `dladdr`.
  2. Calls `com.zettabridge.core.ZBridge.onProxyLoaded(String proxyPath)` through JNI.
     During `JNI_OnLoad`, ART's class-loader override is the plugin loader.
  3. Returns the JNI version reported by the guest (or `JNI_VERSION_1_6`).
- **On failure** `onProxyLoaded` throws `UnsatisfiedLinkError`. The proxy returns
  `JNI_ERR`, so `System.loadLibrary` fails the normal way.

Requirements on Phase 0 (plugin class loader):
- **Class delegation.** Classes in `com.zettabridge.core.*` resolve through the launcher
  class loader, which already loaded `libzbridge.so`. All other classes stay isolated
  (boot parent).
- **Library lookup.** `findLibrary` behaves as in the table above.

### Translator side (`onProxyLoaded`)

1. **Start the guest process** in library mode if it is not running. The arm32 service
   executable `zbhost <target_sdk> <preload>` starts, preloads `libzbcompat.so` and
   `libzbjni.so` with `RTLD_GLOBAL`, and parks in `READY` waiting for requests.
   - A failed preload ends `zbhost` with status 4, and the start fails with that error.
     A `READY` that does not arrive within the timeout (10 s) also fails the start.
   - The runtime is process-lifetime: it is started once per `:guest` process and never
     shut down or restarted (guest threads cannot be torn down).
2. **Load the library.** On a carrier for the calling Java thread
   (`LibraryRuntime::Carrier::load_library`), call guest
   `dlopen(<arm32 lib path>, RTLD_NOW)`. On failure, throw `UnsatisfiedLinkError` with
   the guest `dlerror()` text, read on the same carrier because `dlerror` is per thread.
   - Flags are 32-bit bionic values (`ZB_GUEST_RTLD_*`): `RTLD_NOW` is 0, `RTLD_GLOBAL`
     is 2, and `RTLD_DEFAULT` is `0xffffffff`. Host `<dlfcn.h>` values are wrong for the
     guest (host `RTLD_NOW | RTLD_GLOBAL` is `0x102`, which bionic rejects).
   - Each carrier has its own guest `malloc` string buffer, so concurrent loads never
     share the service thread's scratch buffer.
   - If the calling Java thread already runs guest code (native -> Java -> `loadLibrary`),
     the load runs as a nested call on that thread instead of a new carrier.
3. **Bind `Java_*` exports.**
   - Read the dynamic symbol table of the arm32 ELF on the host and resolve each
     `Java_*` symbol through guest `dlsym`, which keeps the Thumb bit.
   - Decode the mangled name: package/class, method, escapes `_1` `_` / `_2` `;` / `_3`
     `[` / `_0xxxx`, optional `__<signature>` suffix.
   - Find the class with `FindClass` (the class-loader override applies) and its native
     method(s) by reflection: declared methods with the `native` modifier and that name,
     or the exact signature when the suffix is present.
   - Call the real `RegisterNatives` with a host thunk (section 2).
   - Classes missing from the dex are logged once and skipped. Calling such a method
     later throws `UnsatisfiedLinkError` from ART as usual.
4. **Run guest `JNI_OnLoad`** if exported, on the same carrier, with the guest `JavaVM*` (section 3). A version
   it does not accept fails the load. Its `RegisterNatives` calls go through the same thunk
   mechanism:
   - a leading `!` (pre-O fast JNI marker, still accepted by ART) is stripped;
   - each method is registered with its own real `RegisterNatives(..., 1)` call, stopping
     at the first failure with `JNI_ERR` (ART keeps the methods already bound);
   - only the thunk slot of the failed call is released; a slot ART bound is never reused.

Libraries are never unloaded, and `JNI_OnUnload` is not called (same as ART for app
class loaders).

## 2. Java -> guest calls

### Thunk pool

- **The pool.** `libzbridge.so` contains 16384 precompiled arm64 thunks, 8 bytes each:
  `adr x16, .; b zb_native_common`. The common entry derives the slot from the thunk address.
  No code is generated at run time, so there is no W^X or `execmem` dependency.
- **Slot `i`** holds the guest function address (with Thumb bit), the method shorty and
  the static flag.
- **Exhaustion.** Slots are allocated by `RegisterNatives`. If the pool runs out,
  `RegisterNatives` returns `JNI_ERR` and logs.

### Common entry and marshaling

`zb_native_common` (assembly) saves `x0-x7`, `d0-d7` and the incoming stack pointer
(Java stack arguments), then calls the C++ dispatcher with `x16`.

The JNI boundary on 32-bit Android is always AAPCS **softfp**, including hard-float
builds. The dispatcher builds the guest call:

- **`r0`** is the guest `JNIEnv*` of this thread.
- **`r1`** is a local handle for the `jclass` (static method) or `jobject` (instance
  method).
- **Arguments in shorty order:**

  | Shorty | Guest argument |
  |---|---|
  | `Z` `B` `C` `S` `I` | 32 bits; `B`/`S`/`I` sign-extended, `Z`/`C` zero-extended |
  | `F` | IEEE bits in a core register or stack word |
  | `J` `D` | 64 bits on the next even register pair (`r0:r1` / `r2:r3`) or an 8-byte-aligned stack slot |
  | `L` | local handle |

- **Spill rule (AAPCS).** Once a 64-bit value does not fit in the remaining core
  registers, all later arguments go on the stack. The stack is 8-byte aligned at the
  call.
- **Return values:**

  | Shorty | Guest result | Host result |
  |---|---|---|
  | `V` | none | none |
  | `Z` `B` `C` `S` `I` | `r0` | extended per type |
  | `J` `D` | `r0:r1` | 64-bit value |
  | `F` | `r0` bits | float |
  | `L` | handle in `r0` | new host local ref, created before the handle frame closes |

### Which thread runs the guest

- **The host thread already runs a guest thread** (a guest pthread called Java, which
  called native, or a borrower inside a host call): nested host->guest call on its own
  JIT (part 3, "Host-to-guest call"; `LibraryRuntime::call_on_current`).
- **Any other Java thread** (UI thread, `GLThread`, binder threads): borrow a carrier
  (part 3, "Threads"). The binding lasts for the host thread's lifetime and is released
  by a host `pthread_key` destructor.

### Frame, exceptions, crashes

- **Handle frame.** Every Java -> guest call opens a local handle frame. Handles the
  guest created during the call are released when it returns.
- **Java exceptions** raised during the call stay pending in the real `JNIEnv`. ART
  throws them after the native method returns, exactly as for normal native code.
- **Fatal guest signals** produce the translator crash report and end the `:guest`
  process. The launcher process is unaffected.

## 3. Guest JNIEnv: `libzbjni.so` and handle tables

### Guest side

`libzbjni.so` is arm32 C built with the NDK and preloaded by `zbhost`.

- **Tables.** It holds the `JNINativeInterface` (229 slots) and `JNIInvokeInterface`
  (6 slots) tables.
- **Per-thread `JNIEnv`.** Every host thread that enters the guest gets its own guest
  `JNIEnv` object, allocated in guest memory and pointing at the shared table.
- **`Call*Method` with `...` and `va_list`.** These are converted to a `jvalue` array with
  the compiler's own `va_arg`, driven by the method shorty. One host call executes all
  93 variants.
- **Shorty cache.** The `GetMethodID` / `GetStaticMethodID` host calls return the shorty
  together with the id, computed on the host from the signature ART accepted. The guest
  caches id -> shorty, so there is no second descriptor parser. Ids obtained otherwise
  (`FromReflectedMethod`) ask the host once.
- **Buffers.** These functions allocate with guest `malloc`, fill through a host
  `Get*Region` call, and set `*isCopy = JNI_TRUE`:
  - `Get<Prim>ArrayElements`, `GetPrimitiveArrayCritical`;
  - `GetStringChars`, `GetStringUTFChars`, `GetStringCritical`.

  On release:

  | Release mode | Copy back | Free |
  |---|---|---|
  | `0` | yes | yes |
  | `JNI_COMMIT` | yes | no |
  | `JNI_ABORT` | no | yes |

  Copying is allowed by the JNI specification.

### Host side

About 70 flat host calls. Grouped:

| Group | Host calls |
|---|---|
| classes, methods, fields | `FindClass`, `GetSuperclass`, `IsAssignableFrom`, `GetMethodID`/static, `GetFieldID`/static, reflection conversions, `GetMethodShorty` |
| objects | `AllocObject`, `NewObjectA`, `GetObjectClass`, `IsInstanceOf`, `IsSameObject`, `GetObjectRefType` |
| calls | one `CallMethodA(kind, return type, obj/class, methodID, jvalue*)`; kind is virtual, nonvirtual or static |
| fields | `GetField` / `SetField`, instance and static, with a type code |
| strings | `NewString`, `NewStringUTF`, `GetStringLength`, `GetStringUTFLength`, `GetStringRegion`, `GetStringUTFRegion` |
| arrays | `GetArrayLength`, `NewObjectArray`, `Get/SetObjectArrayElement`, `NewPrimArray(type)`, `Get/SetPrimArrayRegion(type)` |
| references | `NewGlobalRef`, `DeleteGlobalRef`, `NewLocalRef`, `DeleteLocalRef`, `NewWeakGlobalRef`, `DeleteWeakGlobalRef`, `EnsureLocalCapacity`, `PushLocalFrame`, `PopLocalFrame` |
| exceptions | `Throw`, `ThrowNew`, `ExceptionOccurred`, `ExceptionDescribe`, `ExceptionClear`, `ExceptionCheck`, `FatalError` |
| other | `MonitorEnter`/`Exit`, `RegisterNatives`, `UnregisterNatives`, direct buffers, `GetJavaVM`, JavaVM operations |

### Handles

- **32-bit values.** `0` is `NULL`. The low 2 bits give the kind (`01` local, `10`
  global, `11` weak global), and the rest is a table index.
- **Local handles.** Per host thread, a stack of frames. The call frame from section 2
  is the base, plus `PushLocalFrame` frames.
- **Global and weak global handles.** Process-wide tables with a free list, under a
  mutex.
- **`jmethodID` / `jfieldID`.** Process-wide, append-only, deduplicated by host id.
  Never freed, since host ids are stable (as in ART).
- **Invalid handles** (wrong kind, freed, out of range) log the call and raise
  `FatalError`, like CheckJNI.

### Direct buffers and JavaVM

- **`NewDirectByteBuffer(guest addr, len)`** creates a real direct buffer at
  `base + addr`.
- **`GetDirectBufferAddress`** returns the guest address when the buffer lies inside the
  guest reservation. Otherwise it returns `NULL` and logs once (see Limits).
- **`GetEnv`** returns this thread's guest `JNIEnv` if attached, else `JNI_EDETACHED`.
- **`AttachCurrentThread` / `AttachCurrentThreadAsDaemon` from a guest thread.** Guest
  threads are real host threads, so the host calls the real function on itself and
  allocates the guest `JNIEnv`.
- **`DetachCurrentThread`** detaches on the host and frees the guest `JNIEnv`.
- **`DestroyJavaVM`** returns `JNI_ERR`.

### Performance fallback ("all on host")

- **The slot table is generated from `jni.h`** by a config listing, per slot, either the
  guest C implementation or a direct `svc` host stub. A host stub decodes arguments on
  the host, including guest `...` / `va_list` walking.
- **Moving a hot slot to the host is a config change.** Guests and other slots are
  unaffected.
- **`ZB_JNI_STATS=1`** counts calls and time per slot and prints the top entries at exit.
- **First candidates:** `Call*MethodV`, `Get/Set<Float|Int>ArrayRegion`,
  `GetPrimitiveArrayCritical`. For 3D titles the bigger costs are expected in GLES
  (part 4) and Dynarmic floating point (`docs/perf-notes.md`). Measure before moving
  anything.

## 4. Errors, limits, testing

### Error handling summary

| Situation | Result |
|---|---|
| guest `dlopen` fails | `UnsatisfiedLinkError` with guest `dlerror` text |
| `Java_*` export without a class | logged once, skipped |
| guest `JNI_OnLoad` returns an unsupported version | `UnsatisfiedLinkError` |
| thunk pool exhausted | `RegisterNatives` returns `JNI_ERR`, logged |
| invalid handle | log + `FatalError` |
| fatal guest signal | crash report, `:guest` process ends |

### Limits of Phase 4

- **Not intercepted.**
  - `System.load("/absolute/path.so")`.
  - Class loaders created by the guest app itself.

  Later fix: rewriting these call sites in dex at import.
- **`GetDirectBufferAddress` on `ByteBuffer.allocateDirect` memory** (outside the guest
  reservation) returns `NULL`. Later fix: mirrored buffers.
- **`NativeActivity` / `ANativeActivity_onCreate`** is not covered: the framework loads
  that library itself. It gets its own spec together with part 4.
- **No ABI mixing** inside one app.
- **`JNI_OnUnload`** is never called.

### Tests on this machine (ctest)

- **`jni_abi_test`**: table-driven AAPCS32 softfp layout from shorties, including
  `(IFFIFF)I`, `(J)V`, `(IJ)V` register-pair skipping, stack spill after a 64-bit
  argument, and all return types.
- **`jni_handles_test`**: encode/decode, frames and `PushLocalFrame`/`PopLocalFrame`,
  global/weak lifetimes, invalid-handle detection.
- **`jni_mangle_test`**: `Java_*` name decoding, including escapes and the `__sig` form.
- **Mock JNI under `zbrun`.** A toy host Java model (a few classes, strings, arrays,
  exceptions) behind the same host-call interface, so `libzbjni.so` runs end to end
  without ART.
  - The guest test `jni_mock_dynamic` covers every `Call*` form and the buffer release
    modes.

### Device test T7 (extends the T6 app)

- **Test harness.**
  - Test dex built here with `d8`.
  - A minimal `BaseDexClassLoader` subclass with the `findLibrary` proxy behavior, so T7
    does not depend on the Phase 0 launcher.
  - arm32 `libjniprobe.so`.
- **Coverage:**
  - every argument and return type;
  - all three `Call*Method` forms;
  - UTF-8 and UTF-16 strings;
  - arrays with commit/abort release;
  - global and weak references;
  - exceptions Java -> guest -> Java;
  - `RegisterNatives` from guest `JNI_OnLoad`;
  - `AttachCurrentThread` from a guest pthread calling Java;
  - native calls from two Java threads at once (carriers);
  - nesting Java -> guest -> Java -> guest.

### Phase 4 acceptance

1. T7 passes on the phone.
2. Orange Roulette smoke test:
   - all 6 libraries load through proxies;
   - `JNI_OnLoad` in `liblime.so` and `libopenal.so` succeed;
   - the 20 native methods are registered;
   - `HXCPP.main()` runs until the first GLES or `AAsset*` host call (not implemented
     yet, logged) with no JNI error.

## Implementation units

| Unit | Language | Purpose |
|---|---|---|
| `android/proxy/zbproxy.c` -> `libzbproxy.so` | C, arm64 | proxy `JNI_OnLoad` |
| `core/src/jni/loader.cpp` | C++ | `onProxyLoaded`: guest process start, `dlopen`, export binding, guest `JNI_OnLoad` |
| `core/src/jni/mangle.cpp` | C++ | `Java_*` name decoding |
| `core/src/jni/thunks.S`, `native_call.cpp` | asm, C++ | thunk pool, common entry, AAPCS32 marshaling |
| `core/src/jni/handles.cpp` | C++ | handle tables |
| `core/src/jni/host_jni.cpp` | C++ | host side of the flat JNI host calls |
| `guest/zbjni/zbjni.c` -> `libzbjni.so` | C, arm32 | guest `JNIEnv`/`JavaVM` tables |
| `tools/gen_jni.py` | Python | slot table and host-call list from `jni.h` plus the host-fallback config |
| `guest/zbhost/zbhost.c` | C, arm32 | library-mode service executable (part 3) |
