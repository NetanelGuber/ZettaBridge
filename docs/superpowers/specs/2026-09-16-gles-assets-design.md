# Part 4: GLES passthrough and Android assets (Phase 5)

Status: design approved 2026-09-16. Builds on
`2026-09-13-guest-system-boundary-design.md` (part 3: host calls, carrier threads, guest
memory) and `2026-09-14-jni-bridge-design.md` (part 1: the host-handler pattern, 32-bit
handles, guest pointer discipline).

## Goal and constraints

Guest arm32 code calls `gl*` and `AAsset*` through the generated stub libraries, which trap
into the translator. Phase 5 turns those traps into real calls on the host's 64-bit GLES
driver and the host `AAssetManager`, so a guest app can render.

Decisions taken before this spec, not to be relitigated:

1. **All of GLES 2.0**, not only the calls Orange Roulette makes. A per-game subset would
   have to be re-measured for every new guest.
2. **`AAsset*` ships in the same phase.** The first test case cannot draw anything without
   its sprites and fonts, so a GLES-only phase could not be accepted.
3. **Correctness first, performance later.** Every `gl*` call goes straight through to the
   real driver on the calling thread. Batching, state caching and call-buffer streaming are
   a later phase; nothing here may make them harder to add, because 3D titles are a goal.

Further constraints, inherited:

- **Generic.** Everything mechanical is generated from the Khronos registry and the NDK
  headers. Nothing is specific to Haxe, lime or Orange Roulette.
- **No host pointer ever reaches the guest.** Host objects the guest must name become
  32-bit handles, as JNI references do.
- **Every guest pointer is bounds-checked** before the driver sees it.
- **No host work inside a Dynarmic callback.** Handlers run after `Run()` returns, from the
  host-call dispatcher, exactly like the JNI bridge.

## Measured surface

Verified on this machine on 2026-09-16 against NDK r29 headers and the Khronos registry
(`https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml`,
2.8 MB, parses with `xml.etree`).

| Fact | Value |
|---|---|
| functions declared in `GLES2/gl2.h` | 142 |
| commands in the registry feature `GL_ES_VERSION_2_0` | 142 |
| in the header but not the feature, or the reverse | 0 and 0 |
| functions with no pointer parameter | 81 |
| functions whose every pointer parameter has `len=` | 57 |
| functions with a pointer and no `len=` | 4 |
| `AAsset*` / `AAssetManager*` / `AAssetDir*` functions in the NDK headers | 19 |

The four pointers without `len=`:

| Function | Pointer | Real rule |
|---|---|---|
| `glBindAttribLocation` | `const GLchar *name` | NUL-terminated string |
| `glGetAttribLocation` | `const GLchar *name` | NUL-terminated string |
| `glGetUniformLocation` | `const GLchar *name` | NUL-terminated string |
| `glVertexAttribPointer` | `const void *pointer` | client-side vertex array, or a buffer offset |

Parameter and return types across all 142 functions:

- Scalars: `GLenum` 98, `GLint` 65, `GLuint` 57, `GLsizei` 53, `GLfloat` 36, `GLboolean` 10,
  `GLsizeiptr` 2, `GLbitfield` 1, `GLintptr` 1. **There is no `GLdouble` and no 64-bit
  parameter anywhere in GLES 2.0.**
- Pointers: 40 input (`const`), 27 functions with at least one output pointer, two double
  pointers (`glShaderSource`, `glGetVertexAttribPointerv`).
- Returns: `void` 128, `GLboolean` 7, `GLenum` 2, `GLint` 2, `GLuint` 2, and one pointer,
  `const GLubyte *glGetString`.

Host-call indices already assigned by `tools/gen_stubs.py` and committed in
`core/src/gen/hostcalls.inc`: **0-141 are `libGLESv2.so`** in alphabetical order, **142-160
are `libandroid.so`**. Phase 5 changes no index; it only adds handlers behind them.

Orange Roulette's measured need (Phase 4 acceptance, `docs/phase4-acceptance.md`): 18
distinct `gl*` calls before the crash, and no `AAsset*` call yet, because rendering setup
runs first.

## Architecture

```
guest arm32 code
  |  call into the generated stub library
guest/stubs/gen/libGLESv2.S:  svc #(0x5A0000 | index) ; bx lr
  |
Process::dispatch_stop  ->  host_call_handler_ chain
  |
HostGl::handle_host_call(index, thread)        core/src/gl/host_gl.cpp
  |  GlCall: AAPCS32 argument reader over GuestThread regs + guest stack
  +-- generated handler                        core/src/gen/gl_dispatch.inc
  |     bounds-checks each pointer, widens GLintptr/GLsizeiptr, calls the real gl*
  +-- hand-written handler (11 functions, below)
  |
real libGLESv2.so of the device, on the calling host thread
```

- **`HostGl` is chained into `LibraryRuntime::set_host_call_handler`** in front of (or
  behind) `HostJni`, exactly as `HostJni` is today. It claims GLES indices 0-141 and returns
  false for everything else; `HostAssets` claims 142-160. A single chain function in the owner (`ProxyRuntime` on
  Android, the test fixture on this machine) calls `HostJni::handle_host_call` first and
  `HostGl::handle_host_call` second; the ranges do not overlap, so the order is free.
- **`HostGl` is process-lifetime**, like `HostJni` and `LibraryRuntime`: one instance, no
  shutdown path.
- **`GlBackend` is the seam**, matching `JniBackend`. The real backend calls the driver
  directly; the test backend (`tests/host/mock_gles.*`) records calls and returns
  predictable ids. Core must not include `GLES2/gl2.h` outside the backend, so that the
  host test build has no GLES dependency.
- **Assets have their own unit**, `HostAssets` (`core/src/gl/host_assets.cpp`), over an
  `AssetBackend` seam: the real one wraps `AAssetManager`, the test one serves a directory
  on disk.

### Generator `tools/gen_gles.py`

Reads `gl.xml` (a copy committed under `third_party/registry/gl.xml`, so a build never
needs the network) plus `GLES2/gl2.h` from the NDK, and writes committed files:

| Output | Content |
|---|---|
| `core/src/gen/gl_dispatch.inc` | one C++ handler per mechanical function plus the `switch` over host-call indices |
| `core/include/zb/gl_hostcalls.h` | `ZB_GL_HC_<name>` index constants, shared by host and tests |
| `core/include/zb/gl_backend.h` | portable GLES typedefs and the typed `GlBackend` seam |
| `core/src/gen/gl_manual.inc` | forward declarations of the hand-written handlers, so a missing one is a link error |

The index order is taken from `tools/gen_stubs.py`, not recomputed, and the generator
**fails** if the two disagree: the guest stub libraries and the host dispatcher must agree
on every index. A ctest `gen_gles_check` runs `tools/gen_gles.py --check` and fails when a
committed output is stale, exactly like `gen_jni_check`.

### Why generated code and not a table

A table of (argument kinds, lengths) interpreted at run time would need its own interpreter
for `len=` products and `COMPSIZE`, and would defeat the later optimization phase. Generated
straight-line C++ per function is what the JNI thunks already do, compiles to a direct call,
and is where a future fast path (batching, state cache) will be inserted per function.

## Marshaling rules

Every GLES 2.0 parameter is exactly one 32-bit guest word, so argument *n* is guest register
`r0..r3` for n < 4 and the guest stack word at `sp + 4*(n-4)` above that. This is the same
`JniCall::arg` rule as the JNI bridge, and it needs no AAPCS 64-bit-pair handling at all,
because no GLES 2.0 function takes a 64-bit value.

| Class | Count | Registry evidence | Rule |
|---|---|---|---|
| scalar | all non-pointer parameters | no `*` in the `<param>` text | read one guest word, cast to the `<ptype>` |
| pointer size | `GLintptr`, `GLsizeiptr` | `<ptype>` | read one guest word, **widen** to the host's 8-byte type; `GLintptr` sign-extends |
| input array | 40 pointers | `const` and a `len=` | bounds-check `len * sizeof(base)` for read, pass `base() + addr` straight to the driver |
| output array | 27 functions | non-`const` and a `len=` | bounds-check for write, pass `base() + addr` straight to the driver |
| NUL-terminated string | 3 | no `len=`, `const GLchar *` | scan guest memory to the terminator with a bound, pass the host address |
| literal length | `len="1".."4"` | the literal | the constant |
| parameter length | `len="count"`, `n`, `bufSize`, `size`, `maxCount`, `length`, `imageSize` | that parameter's value | that argument, read as 64 bits |
| product length | `len="count*2"`, `*3`, `*4`, `*9`, `*16` | the product | argument times the literal |
| `COMPSIZE(...)` | 18 pointers | opaque to the registry | hand-written rule, below |

**Guest memory is host memory.** A guest address `a` is the host address `base() + a`
inside one 4 GiB reservation, so an input or output array needs a bounds check and nothing
else: no copy, no staging buffer. This is the single largest simplification of Phase 5 and
it is only true because `GuestMemory` owns a flat reservation. The consequences:

- A copy is needed only where the *layout* differs, which in the whole of GLES 2.0 is the
  two double pointers and nothing else.
- `GuestMemory::host_ptr(addr, bytes, kPageRead | kPageWrite)` performs the check and
  returns the pointer; a null result is a guest error (below), never a driver call.
- A null guest pointer stays null: GLES defines `NULL` for `glTexImage2D`, `glBufferData`,
  `glDrawElements` (with a bound element buffer) and `glGetShaderSource`'s `length`.
- The driver must not retain the pointer past the call. That is true of every GLES 2.0
  entry point except client-side vertex arrays, which is exactly why those are handled
  separately.

### The 11 hand-written handlers

The registry describes shapes, not semantics. These eleven need real rules; every other
function in GLES 2.0 is generated.

| Function | Why the registry is not enough |
|---|---|
| `glVertexAttribPointer` | `pointer` is a client-array pointer or a buffer offset, decided by the current `GL_ARRAY_BUFFER` binding |
| `glDrawArrays` | must materialize the enabled client arrays for `[first, first+count)` |
| `glDrawElements` | same, over the index range; `indices` is itself a pointer or an offset, and `len=COMPSIZE(count,type)` |
| `glShaderSource` | `const GLchar *const*`: an array of **32-bit** guest pointers; the host needs 64-bit ones. String lengths come from `length[i]` or the terminator |
| `glGetVertexAttribPointerv` | must return the **guest** pointer the guest passed in, never the host one |
| `glGetString` | returns a host pointer; the guest needs a guest-memory copy with process lifetime |
| `glGetUniformfv`, `glGetUniformiv` | `len=COMPSIZE(program,location)`: the element count depends on the uniform's declared type |
| `glTexImage2D`, `glTexSubImage2D`, `glReadPixels` | `len=COMPSIZE(format,type,width,height)` also depends on `GL_UNPACK_ALIGNMENT` / `GL_PACK_ALIGNMENT` |

The other `COMPSIZE` shapes are handled by generated code plus one small table:

- **`COMPSIZE(pname)` on `glGetBooleanv` / `glGetFloatv` / `glGetIntegerv`.** A hand-written
  `gl_pname_count(pname)` table. Everything in GLES 2.0 is 1 element except
  `GL_ALIASED_LINE_WIDTH_RANGE`, `GL_ALIASED_POINT_SIZE_RANGE`, `GL_DEPTH_RANGE`,
  `GL_MAX_VIEWPORT_DIMS` (2), `GL_BLEND_COLOR`, `GL_COLOR_CLEAR_VALUE`,
  `GL_COLOR_WRITEMASK`, `GL_SCISSOR_BOX`, `GL_VIEWPORT` (4), and the two variable-length
  queries `GL_COMPRESSED_TEXTURE_FORMATS` and `GL_SHADER_BINARY_FORMATS`, whose count is
  read first from `GL_NUM_COMPRESSED_TEXTURE_FORMATS` / `GL_NUM_SHADER_BINARY_FORMATS`.
  An unknown `pname` is 1, logged once, and the driver's own `GL_INVALID_ENUM` follows.
- **`COMPSIZE(pname)` on the other nine `glGet*` and `glTexParameter*v` functions** is
  always 1 element in GLES 2.0 (no ES2 pname of theirs is a vector). The generator emits 1
  for them from an explicit list, so adding ES3 later is a list change, not a rewrite.

### Client-side vertex arrays

GLES 2.0 allows vertex data in client memory, which a guest will use. The driver reads that
memory **during the draw call**, so the guest addresses must be valid host addresses at
that moment - which they are, since guest memory is host memory - but the attributes must
also be re-pointed, because a guest pointer is not a host pointer.

`HostGl` keeps per-context attribute state:

| Field | Source |
|---|---|
| `enabled` | `glEnableVertexAttribArray` / `glDisableVertexAttribArray` |
| `size`, `type`, `normalized`, `stride` | `glVertexAttribPointer` |
| `guest_pointer` | `glVertexAttribPointer`, recorded verbatim |
| `buffer` | the `GL_ARRAY_BUFFER` binding at the time of the call |

- **With a non-zero `GL_ARRAY_BUFFER` binding**, `pointer` is an offset. The call is
  forwarded unchanged and nothing is recorded beyond the binding.
- **With binding 0**, the call is *not* forwarded. The state is recorded, and at draw time
  each enabled client attribute is re-issued as
  `glVertexAttribPointer(index, size, type, normalized, stride, base() + guest_pointer)`
  after the byte range has been bounds-checked.
- **Byte range.** With `stride == 0` the effective stride is the element size
  (`size * sizeof(type)`, with the GLES 2.0 packed types excluded from ES2's attribute
  types). The range is `[first*stride, (first + count - 1)*stride + element_size)` for
  `glDrawArrays`; for `glDrawElements` it is computed from the **maximum index** in the
  index range, which means reading the indices - from guest memory when `indices` is a
  client pointer, or from the bound `GL_ELEMENT_ARRAY_BUFFER`, which cannot be read back in
  GLES 2.0 and is therefore trusted (see Limits).
- **`glGetVertexAttribPointerv`** answers from this state, returning the recorded guest
  pointer, never a host address.

This "record and materialize at draw" shape is deliberately the same shape a later batching
phase needs, so the optimization work replaces the draw-time function and nothing else.

## Threading

- **Guest GL calls run inline on the calling host thread.** A Java `GLThread` that entered
  the guest borrowed a carrier (part 3, "Threads"), so the guest code executes on
  `GLThread` itself, where the EGL context created Java-side by `GLSurfaceView` is already
  current. Calling the driver from the handler therefore calls it on the right thread with
  the right context. **There is no cross-thread dispatch and no GL command queue.**
- **No EGL is translated.** `egl*` is not in the stub libraries and is not added: contexts,
  surfaces and `eglSwapBuffers` stay on the Java side.
- **Per-context state** (attributes, pixel-store alignment, the `glGetString` cache) is kept
  in a `thread_local` block, keyed by nothing else. GLES 2.0 state is per context, a context
  is current on one thread at a time, and a guest with two GL threads would need per-context
  keying; that is a Limit, logged once when a second thread issues GL calls.
- **A GL call from a thread with no current context** is the driver's problem, not ours: it
  is forwarded and the driver reports `GL_INVALID_OPERATION` or crashes exactly as it would
  for native code. We do not query `eglGetCurrentContext` per call.

## Error handling

The rule is: **a broken guest must not corrupt the host, and must not silently render
wrong.**

| Situation | Result |
|---|---|
| guest pointer out of range, unmapped, or lacking the needed permission | log once per call site, record in `RuntimeReport`, set `GL_INVALID_VALUE` through the backend, do **not** call the driver |
| length computed from guest arguments overflows 64 bits, or exceeds the guest space | same |
| NUL-terminated string with no terminator within its bound | same |
| unknown `pname` in `gl_pname_count` | count 1, logged once |
| client vertex array whose byte range is unreadable | the draw call is dropped, logged once, `GL_INVALID_OPERATION` |
| a host call index inside 0-160 with no handler | log and abort: the generator guarantees coverage, so this is a build error that escaped |
| real GL errors | not intercepted; `glGetError` is a normal generated passthrough |

- **Errors never abort the guest.** Unlike the JNI bridge, where an invalid handle means the
  guest has already corrupted itself, a bad GL pointer is ordinary application error
  territory, and GLES has an error channel. The guest survives and can be debugged.
- **`RuntimeReport` gains a GL section**: total GL calls, the first rejected call with its
  reason, and the distinct rejected calls with counts, bounded like the existing lists. On
  the device that file is the only diagnostic that survives, because OxygenOS drops
  third-party logcat output.
- **`ZB_GL_TRACE=1`** logs every GL call with its arguments, once per call, for bring-up.
  It is off by default and costs one branch per call.

## Assets

`libandroid.so` stub indices 142-160 are 19 functions. Orange Roulette needs six of them
(`AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_read`, `AAsset_seek`,
`AAsset_getLength`, `AAsset_close`); Phase 5 implements all 19, for the same reason GLES is
implemented in full.

- **Handles, not pointers.** `AAssetManager*`, `AAsset*` and `AAssetDir*` are host pointers.
  The guest gets a 32-bit handle from a table with a free list, under a mutex, following
  `core/src/jni/jni_handles.cpp`. A handle carries a kind tag, so passing an `AAssetDir`
  handle to `AAsset_read` is detected. Handle 0 is `NULL`. An invalid handle is logged and
  returns the C error value of that function (`NULL`, `-1` or `0`), never `FatalError`:
  asset code is guest application code.
- **`AAssetManager_fromJava(JNIEnv*, jobject)`** is the one asset function that crosses the
  JNI bridge. Its guest arguments are a guest `JNIEnv*` and a **32-bit JNI handle**.
  `HostAssets` resolves the handle through `HostJni` to a host `jobject` and calls the real
  `AAssetManager_fromJava` with the calling thread's real `JNIEnv`. It therefore needs a
  reference to `HostJni`, which is the only coupling between the two units.
- **Sizes.** `AAsset_getLength`/`getRemainingLength` return `off_t`, 4 bytes on the guest and
  8 on the host; a length above `INT32_MAX` is clamped to `INT32_MAX` and logged once, as
  bionic's own 32-bit `off_t` path does. The `*64` variants return the true value in
  `r0:r1`, per AAPCS.
- **`AAsset_getBuffer`** returns a host pointer to mapped APK data, which the guest cannot
  use. The buffer is copied into guest memory allocated through the guest allocator (the
  `LibraryRuntime` service `malloc`), cached per open asset, and freed by `AAsset_close`.
  An asset larger than the remaining guest address space returns `NULL` and is logged. This
  is the one asset call with a real cost, and it is the one hxcpp/lime uses for whole-file
  reads.
- **`AAsset_openFileDescriptor`** returns a real fd, which guest code can use through the
  syscall layer unchanged, plus `off_t` out-parameters written into guest memory.
- **`AAssetDir_getNextFileName`** returns a host `const char*` with asset-dir lifetime; it
  is copied into a per-`AAssetDir` guest buffer that is reused on the next call, matching
  the NDK's own "valid until the next call" contract.

## Limits of Phase 5

- **GLES 2.0 only.** No ES 3.x, no extension entry points. A guest that resolves an ES3
  function through `eglGetProcAddress` gets nothing, because `eglGetProcAddress` is not
  translated and the stub library exports only the 142 core names.
- **One GL thread.** Per-context state is per host thread; a guest that makes the same
  context current on two threads in turn is not supported. Logged once.
- **`glDrawElements` with a bound element-array buffer** cannot have its maximum index
  checked, because GLES 2.0 has no buffer read-back. The client-array byte range is then
  computed from `count` alone under the assumption that indices are below `count`, which is
  wrong for an arbitrary index buffer. Mitigation: when any client array is enabled **and**
  an element buffer is bound, the whole enabled client array range is bounds-checked up to
  the mapped end of its page range instead. A guest that combines the two is rare; it is
  logged once.
- **No `glMapBuffer`**, which is not in GLES 2.0.
- **No performance work.** Each call is one Dynarmic halt plus one driver call. Orange
  Roulette makes tens of calls per frame, which is fine; a 3D title making thousands will
  need the batching phase.
- **No compressed-texture validation.** `glCompressedTexImage2D`'s `imageSize` is the
  guest's word and is bounds-checked, but not checked against the format.
- **Assets from a non-APK source** (`AAssetManager` over an OBB or a split) are whatever
  the host framework hands us; nothing extra is done.

## Testing

Everything except the real EGL context, `GLThread` and the first frame runs in ctest on this
machine.

- **`gles_marshal_test`.** Pure host unit test of the generated marshaling: argument slot
  reading including stack arguments, `GLintptr`/`GLsizeiptr` widening, every `len=` shape
  (literal, parameter, product), the `gl_pname_count` table, and the pixel-size computation
  with each `GL_UNPACK_ALIGNMENT`.
- **`mock_gles`** (`tests/host/mock_gles.{h,cpp}`) implements `GlBackend`: it records every
  call with its arguments and a copy of every input array, returns predictable ids
  (`glCreateShader` -> 1, 2, 3...), and answers `glGet*` from a small state model. It is the
  GLES analogue of `MockJvm`.
- **`gles_bridge_test`** is the real one, modeled on `jni_bridge_test`: an arm32 guest probe
  `guest/testlib/zbglprobe.c`, built against the **generated stub library**, is loaded into a
  `LibraryRuntime` and drives the true guest path (stub -> `svc` -> dispatcher -> handler ->
  mock). It covers:
  - one call of each of the 142 functions, with the mock asserting the arguments arrived
    intact, including calls with more than four words of arguments;
  - `glShaderSource` with several strings, with and without a `length` array, and with an
    embedded NUL;
  - `glGetShaderiv` / `glGetProgramiv` / `glGetIntegerv` output values reaching guest memory;
  - the three name lookups with a long name and an unterminated name;
  - `glGetString` for every enum, twice, checking the guest gets a stable guest pointer;
  - `glGetVertexAttribPointerv` returning the guest pointer the guest passed in.
- **`gles_pointer_test`.** The abuse cases, each asserting the driver was **not** called and
  a GL error was set: an address past the guest space, an unmapped address, a read-only
  address as an output array, a length that overflows, an absurd `count` in
  `glGenTextures`, `glBufferData` with a size larger than guest memory, a string with no
  terminator, and a null pointer where GLES allows one (which must be forwarded).
- **`gles_client_arrays_test`.** Client-side vertex arrays: `glDrawArrays` with a non-zero
  `first`, with stride 0 and with a stride larger than the element, several enabled
  attributes at once, an attribute pointing at a buffer while another is client-side,
  `glDrawElements` with `GL_UNSIGNED_BYTE` and `GL_UNSIGNED_SHORT` indices from client
  memory, and the same with the element buffer bound.
- **`assets_test`.** `HostAssets` over a directory-backed `AssetBackend`: open, read, seek,
  length, `getBuffer` (including the guest copy and its release at close), directory
  enumeration, `openFileDescriptor`, handle validity and kind confusion, and a length above
  `INT32_MAX` through a sparse file.
- **`gen_gles_check`.** `tools/gen_gles.py --check` fails when a committed generated file is
  stale, and fails when `gen_stubs.py` and `gen_gles.py` disagree on any index.

On the device, the Phase 5 test is Orange Roulette itself; there is no separate T8 app. The
launcher's runtime report carries the GL section, so a failed run says which call was
rejected and why.

## Acceptance

1. On this machine: the whole host suite passes, including the six new tests, the guest
   suite still passes, and `ninja -C build/android-arm64 zbridge` links with the real GLES
   backend.
2. On the OnePlus 13: **Orange Roulette draws its intro screen** from a pinned shortcut,
   through the production launcher, with no rejected GL call and no `AAsset*` failure in the
   runtime report.

Phase 6 (input, audio, the rest of the asset surface in anger) follows. Sound and touch are
explicitly *not* part of this acceptance; a silent, non-interactive intro screen passes.

## Implementation units

| Unit | Language | Purpose |
|---|---|---|
| `tools/gen_gles.py` | Python | generator: dispatch, indices, manual declarations, `--check` |
| `third_party/registry/gl.xml` | data | the pinned Khronos registry copy |
| `core/include/zb/host_gl.h`, `core/src/gl/host_gl.cpp` | C++ | `HostGl`, the `GlCall` argument reader, the host-call switch |
| `core/src/gen/gl_dispatch.inc`, `core/include/zb/gl_hostcalls.h` | generated | the 131 mechanical handlers and the index constants |
| `core/src/gl/gl_manual.cpp` | C++ | the 11 hand-written handlers and `gl_pname_count` |
| `core/src/gl/gl_client_arrays.cpp` | C++ | attribute state and draw-time materialization |
| `core/include/zb/gl_backend.h` | generated C++ | the driver seam and portable GLES types |
| `core/android/gl_driver_backend.cpp` | C++ | the real GLES backend (Android build) |
| `core/include/zb/host_assets.h`, `core/src/gl/host_assets.cpp` | C++ | `HostAssets`, asset handles |
| `core/android/asset_backend.cpp` | C++ | the real `AAssetManager` backend |
| `tests/host/mock_gles.{h,cpp}`, `tests/host/fake_assets.{h,cpp}` | C++ | the test backends |
| `guest/testlib/zbglprobe.c` | C, arm32 | the guest probe that drives the real path |
