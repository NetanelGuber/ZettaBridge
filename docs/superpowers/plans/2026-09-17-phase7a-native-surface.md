# Phase 7 part 1: native surface (EGL and ANativeWindow) implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A guest library creates its own EGL context on a window it gets from Java, draws, and
presents frames, so Flutter and (later) `NativeActivity` guests can run.

**Architecture:** Guest `libEGL.so` and the `ANativeWindow_*` half of guest `libandroid.so` are
generated trap stubs. `HostEgl` and `HostNativeWindow` live in `core/` behind abstract backends
(`EglBackend`, `NativeWindowBackend`); the real implementations live in `core/android/`. Every
EGL and window object crosses to the guest only as a 32-bit handle. This mirrors Phase 5
(`HostGl`/`GlBackend`) and Phase 5 Task 7 (`HostAssets`/`AssetBackend`) exactly.

**Tech stack:** C++20, CMake+Ninja, Python generators, NDK r29 arm32 assembly stubs, ctest.

**Spec:** `docs/superpowers/specs/2026-09-17-native-surface-design.md`.

## Global constraints

- Host-call indices are append-only. ALooper keeps indices 220-227.
- Guest fds are real fds in the launcher process; never translate or close registered fds.
- Never hold the looper mutex across `poll()` or a nested guest callback.
- Guest callbacks run only through `LibraryRuntime::call_on_current`.
- Keep NativeActivity lifecycle, input queues and `AConfiguration` out of this plan.
- Every task updates `AGENTS.md` and ends in its own local commit; do not push.

---

## File structure

Created:
- `third_party/registry/egl.xml` — pinned Khronos registry.
- `tools/gen_egl.py` — generator, modelled on `tools/gen_gles.py`.
- `core/include/zb/egl_hostcalls.h`, `core/include/zb/egl_backend.h`,
  `core/src/gen/egl_dispatch.inc` — generated, committed.
- `core/include/zb/host_egl.h`, `core/src/gl/host_egl.cpp` — EGL dispatcher, handle tables.
- `core/src/gl/egl_manual.cpp` — the hand-written EGL cases.
- `core/include/zb/native_window_backend.h`, `core/include/zb/host_native_window.h`,
  `core/src/android/host_native_window.cpp` — window host calls (portable half).
- `core/android/egl_driver_backend.{h,cpp}`, `core/android/native_window_driver_backend.{h,cpp}`
  — real device backends.
- `tests/host/mock_egl.{h,cpp}`, `tests/host/mock_native_window.h` — test seams.
- `tests/host/egl_marshal_test.cpp`, `tests/host/native_window_test.cpp`,
  `tests/host/egl_chain_test.cpp` — host tests.
- `guest/tests/zbeglprobe.c` — guest end-to-end probe.

Modified:
- `tools/gen_stubs.py` — adds `libEGL` and the `ANativeWindow_*` names.
- `core/src/gen/hostcalls.inc`, `guest/stubs/gen/*.S` — regenerated.
- `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`, `tools/build_guest.sh`.
- `core/include/zb/proxy_runtime.h`, `core/src/jni/proxy_runtime.cpp` — chaining.
- `core/include/zb/runtime_report.h`, `core/src/runtime_report.cpp` — EGL section.
- `core/android/guest_jni_runtime.h` — pass the new backends.
- `AGENTS.md`, `CLAUDE.md` — state and gotchas.

**Index stability rule (applies to every task):** `core/include/zb/asset_hostcalls.h` holds
hand-written literal indices (145-157), and `core/include/zb/host_gl.h` documents GLES as 0-141.
New names must therefore be **appended**, never merged into an existing sorted list:
`android_names()` returns `sorted(AAsset*) + sorted(ANativeWindow*)`, and `libEGL` comes after
`libandroid` in `LIBRARIES`.

---

### Task 1: Guest stubs for EGL and ANativeWindow

**Files:**
- Create: `third_party/registry/egl.xml`
- Modify: `tools/gen_stubs.py`
- Regenerate: `guest/stubs/gen/libEGL.S`, `guest/stubs/gen/libandroid.S`,
  `core/src/gen/hostcalls.inc`

- [ ] **Step 1: Pin the registry**

```bash
curl -fsSL https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/egl.xml -o third_party/registry/egl.xml; grep -c "<command" third_party/registry/egl.xml
```
Expected: a count above 80. If the machine has no network, fetch it on the phone and copy it in;
do not hand-write the file.

- [ ] **Step 2: Extend the generator**

In `tools/gen_stubs.py`, replace `android_asset_names` with two functions and update
`LIBRARIES`. Only `EGL/egl.h` is stubbed: Android's `libEGL.so` exports the core entry points,
and extensions are reached through `eglGetProcAddress`.

```python
def android_asset_names():
    names = set()
    decl = re.compile(r"^[A-Za-z_][\w \*]*\b(AAsset\w*)\(")
    for header in ("asset_manager.h", "asset_manager_jni.h"):
        for line in open(os.path.join(INCLUDE, "android", header)):
            m = decl.match(line)
            if m:
                names.add(m.group(1))
    return sorted(names)


# Appended after the AAsset* names so the hand-written indices in
# core/include/zb/asset_hostcalls.h (145-157) keep pointing at the same functions.
ANATIVE_WINDOW = [
    "ANativeWindow_acquire",
    "ANativeWindow_fromSurface",
    "ANativeWindow_getFormat",
    "ANativeWindow_getHeight",
    "ANativeWindow_getWidth",
    "ANativeWindow_release",
    "ANativeWindow_setBuffersGeometry",
    "ANativeWindow_toSurface",
]


def android_names():
    return android_asset_names() + ANATIVE_WINDOW


def egl_names():
    text = open(os.path.join(INCLUDE, "EGL", "egl.h")).read()
    return sorted(set(re.findall(r"EGLAPI\s+[^;]*?EGLAPIENTRY\s+(egl\w+)\s*\(", text)))


LIBRARIES = [
    ("libGLESv2", gles2_names),
    ("libandroid", android_names),
    ("libEGL", egl_names),
]
```

- [ ] **Step 3: Regenerate and check the index rule**

```bash
python3 tools/gen_stubs.py; grep -n "AAssetManager_fromJava\|AAsset_read\|ANativeWindow_acquire\|eglCreateContext" core/src/gen/hostcalls.inc
```
Expected: `AAssetManager_fromJava` is still 145 and `AAsset_read` still 157; `ANativeWindow_*`
starts at 160; `egl*` starts after the window names. If the asset indices moved, the append rule
was broken — fix the generator, do not edit `asset_hostcalls.h`.

- [ ] **Step 4: Build the guest tree**

```bash
tools/build_guest.sh; ls -la build/guest/lib/libEGL.so build/guest/lib/libandroid.so
```
Expected: both exist. `tools/build_guest.sh` must learn `libEGL` the same way it builds
`libGLESv2`; add it there if the build fails with "no rule".

- [ ] **Step 5: Verify the stub symbols**

```bash
nm -D --defined-only build/guest/lib/libEGL.so | grep -c " T egl"; nm -D --defined-only build/guest/lib/libandroid.so | grep -c " T ANativeWindow"
```
Expected: the EGL count matches the generator's printed count; the window count is 8.

- [ ] **Step 6: Commit**

```bash
git add third_party/registry/egl.xml tools/gen_stubs.py tools/build_guest.sh guest/stubs/gen core/src/gen/hostcalls.inc; git commit -m "stubs: guest libEGL.so and ANativeWindow_* trap stubs"
```

---

### Task 2: `tools/gen_egl.py` and the generated protocol

**Files:**
- Create: `tools/gen_egl.py`
- Create (generated, committed): `core/include/zb/egl_hostcalls.h`,
  `core/include/zb/egl_backend.h`, `core/src/gen/egl_dispatch.inc`
- Modify: `tests/host/CMakeLists.txt`

Read `tools/gen_gles.py` first and follow its structure: it parses the registry, joins it with
the stub index table, and writes a `k*HostCalls` table, a virtual backend class and a dispatcher
`switch`. This task produces the same three outputs for EGL.

- [ ] **Step 1: Write the generator**

`tools/gen_egl.py` reads `third_party/registry/egl.xml` and `core/src/gen/hostcalls.inc`, and
emits:
- `egl_hostcalls.h`: `ZB_EGL_HC_<name>` constants, `kEglHostCalls` (index -> name),
  `kEglHostCallFirst` / `kEglHostCallLast`;
- `egl_backend.h`: `class EglBackend` with one virtual per EGL function, each defaulting to
  `invoke(name, {words...})` exactly as `GlBackend` does, plus the EGL scalar typedefs
  (`EGLint`, `EGLenum`, `EGLBoolean`, and `EGLDisplay`/`EGLConfig`/`EGLContext`/`EGLSurface` as
  `void*`);
- `egl_dispatch.inc`: `bool HostEgl::dispatch(Call& call)` with one `case` per mechanical
  function; every function in `HANDWRITTEN` instead calls `zbegl_manual_<name>(*this, call)`.

```python
HANDWRITTEN = {
    "eglChooseConfig",
    "eglCreateWindowSurface",
    "eglGetConfigAttrib",
    "eglGetConfigs",
    "eglGetCurrentContext",
    "eglGetCurrentDisplay",
    "eglGetCurrentSurface",
    "eglGetProcAddress",
    "eglQueryString",
    "eglQuerySurface",
    "eglQueryContext",
    "eglSwapBuffers",
}
```

Handle-typed parameters (`EGLDisplay`, `EGLConfig`, `EGLContext`, `EGLSurface`,
`EGLNativeWindowType`) are **not** mechanical scalars: the generated case calls
`call.handle<EGLDisplay>(position)` (Task 3), which fails the call with `EGL_BAD_DISPLAY` /
`EGL_BAD_CONTEXT` / `EGL_BAD_SURFACE` / `EGL_BAD_PARAMETER` when the handle is unknown. A
handle-typed **return** goes through `call.set_handle(value)`.

- [ ] **Step 2: Run it and read the output**

```bash
python3 tools/gen_egl.py; head -40 core/src/gen/egl_dispatch.inc; grep -c "case ZB_EGL_HC_" core/src/gen/egl_dispatch.inc
```
Expected: one case per stubbed EGL function, and the file compiles later in Task 3.

- [ ] **Step 3: Add the staleness test**

In `tests/host/CMakeLists.txt`, next to `gen_gles_check`:

```cmake
add_test(NAME gen_egl_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_egl.py --check)
```

- [ ] **Step 4: Run it**

```bash
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; ctest --test-dir build/host -R gen_egl_check --output-on-failure
```
Expected: PASS. Then touch a generated file, rerun, and confirm it FAILS; restore it.

- [ ] **Step 5: Commit**

```bash
git add tools/gen_egl.py core/include/zb/egl_hostcalls.h core/include/zb/egl_backend.h core/src/gen/egl_dispatch.inc tests/host/CMakeLists.txt; git commit -m "gles: generate the EGL host-call protocol from egl.xml"
```

---

### Task 3: `HostEgl` core and handle tables

**Files:**
- Create: `core/include/zb/host_egl.h`, `core/src/gl/host_egl.cpp`
- Create: `tests/host/mock_egl.h`, `tests/host/mock_egl.cpp`, `tests/host/egl_marshal_test.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

`HostEgl::Call` is the same shape as `HostGl::Call` (`core/include/zb/host_gl.h`): it captures
r0-r3, reads further arguments from the guest stack, translates guest pointers with bounds
checks, and `fail(error, reason)` records a rejection instead of calling the backend. Reuse that
file as the template; the only new parts are the handle helpers below.

- [ ] **Step 1: Write the failing test**

`tests/host/egl_marshal_test.cpp`, following `tests/host/gles_marshal_test.cpp`:

```cpp
// A display handle is stable, never 0, and an unknown handle never reaches the backend.
static void check_handles(zb::HostEgl& egl, MockEgl& backend) {
    const std::uint32_t display = call_egl(egl, ZB_EGL_HC_eglGetDisplay, {0});
    CHECK(display != 0);
    CHECK(call_egl(egl, ZB_EGL_HC_eglGetDisplay, {0}) == display);  // same host value, same handle

    backend.calls().clear();
    CHECK(call_egl(egl, ZB_EGL_HC_eglInitialize, {display + 1000, 0, 0}) == 0);  // EGL_FALSE
    CHECK(backend.calls().empty());
    CHECK(call_egl(egl, ZB_EGL_HC_eglGetError, {}) == 0x3008);  // EGL_BAD_DISPLAY
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
ninja -C build/host egl_marshal_test
```
Expected: FAIL to compile, "zb/host_egl.h: No such file".

- [ ] **Step 3: Implement `HostEgl`**

`core/include/zb/host_egl.h` declares:

```cpp
class HostEgl {
public:
    class Call;  // same API as HostGl::Call, plus:
    //   template <typename T> T handle(unsigned position);  // guest handle -> host pointer
    //   void set_handle(const void* value);                 // host pointer -> guest handle
    HostEgl(LibraryRuntime& runtime, EglBackend& backend) : runtime_(runtime), backend_(backend) {}
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    EglBackend& backend() { return backend_; }
    LibraryRuntime& runtime() { return runtime_; }
    // 0 for nullptr (EGL_NO_*), a stable handle otherwise. The same host pointer always maps to
    // the same handle, so guest equality comparisons behave like a real driver's.
    std::uint32_t handle_for(const void* value);
    const void* value_for(std::uint32_t handle) const;
private:
    bool dispatch(Call& call);      // core/src/gen/egl_dispatch.inc
    LibraryRuntime& runtime_;
    EglBackend& backend_;
    GlobalHandles objects_{HandleKind::Global};
    std::unordered_map<const void*, std::uint32_t> by_value_;
    mutable std::mutex mutex_;
};
```

`core/src/gl/host_egl.cpp` implements `Call`, `handle_for`, `value_for` and
`handle_host_call`, and `#include "gen/egl_dispatch.inc"` at the end, the way
`core/src/gl/host_gl.cpp` includes `gen/gl_dispatch.inc`.

- [ ] **Step 4: Run the test**

```bash
ninja -C build/host; ctest --test-dir build/host -R egl_marshal_test --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/host_egl.h core/src/gl/host_egl.cpp core/CMakeLists.txt tests/host/mock_egl.h tests/host/mock_egl.cpp tests/host/egl_marshal_test.cpp tests/host/CMakeLists.txt; git commit -m "egl: HostEgl dispatcher with 32-bit object handles"
```

---

### Task 4: The hand-written EGL cases

**Files:**
- Create: `core/src/gl/egl_manual.cpp`
- Modify: `tests/host/egl_marshal_test.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `egl_marshal_test.cpp`:

```cpp
// eglChooseConfig writes handles, not host pointers, into the guest array and honours the size.
static void check_choose_config(zb::HostEgl& egl, MockEgl& backend, Guest& guest) {
    const std::uint32_t attribs = guest.write_words({0x3024, 8, 0x3038});  // RED_SIZE 8, NONE
    const std::uint32_t configs = guest.zeroed_words(4);
    const std::uint32_t count = guest.zeroed_words(1);
    backend.set_configs({reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000)});
    CHECK(call_egl(egl, ZB_EGL_HC_eglChooseConfig, {display, attribs, configs, 4, count}) == 1);
    CHECK(guest.word(count) == 2);
    CHECK(guest.word(configs) != 0 && guest.word(configs) < 0x10000);      // a handle
    CHECK(egl.value_for(guest.word(configs)) == reinterpret_cast<void*>(0x1000));
}

// eglGetProcAddress answers with the guest stub of a name we generate, and 0 otherwise.
static void check_proc_address(zb::HostEgl& egl) {
    CHECK(call_egl(egl, ZB_EGL_HC_eglGetProcAddress, {name_of("eglCreateContext")}) != 0);
    CHECK(call_egl(egl, ZB_EGL_HC_eglGetProcAddress, {name_of("eglNoSuchThing")}) == 0);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
ninja -C build/host egl_marshal_test; ctest --test-dir build/host -R egl_marshal_test --output-on-failure
```
Expected: FAIL (undefined `zbegl_manual_eglChooseConfig`, or wrong values).

- [ ] **Step 3: Implement the cases**

In `core/src/gl/egl_manual.cpp`, one `zbegl_manual_<name>` per entry of `HANDWRITTEN`:
- `eglChooseConfig` / `eglGetConfigs`: bounds-check the guest array of `config_size` words, call
  the backend into a host vector, then write one handle per returned config and the count;
- `eglGetConfigAttrib`, `eglQuerySurface`, `eglQueryContext`: one `EGLint` out-parameter, checked
  for writability;
- `eglQueryString`: copy the host string into guest memory through the guest allocator, cache one
  copy per (display, name) pair, and return the guest address — never the host pointer;
- `eglGetProcAddress`: read the guest string, look the name up in `kEglHostCalls`, and return the
  guest stub address resolved with `runtime().find_symbol` in `libEGL.so`; 0 when not found;
- `eglCreateWindowSurface`: the window argument is a `HostNativeWindow` handle (Task 5), resolved
  through the window table, `EGL_BAD_NATIVE_WINDOW` when unknown;
- `eglGetCurrentDisplay/Context/Surface`: call the backend and map the host value back with
  `handle_for`;
- `eglSwapBuffers`: pass through and count in the report (Task 7).

- [ ] **Step 4: Run the tests**

```bash
ctest --test-dir build/host -R egl_marshal_test --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/src/gl/egl_manual.cpp tests/host/egl_marshal_test.cpp; git commit -m "egl: hand-written cases for config arrays, strings and proc addresses"
```

---

### Task 5: `HostNativeWindow`

**Files:**
- Create: `core/include/zb/native_window_backend.h`, `core/include/zb/host_native_window.h`,
  `core/src/android/host_native_window.cpp`
- Create: `core/include/zb/window_hostcalls.h` - the eight `ZB_WINDOW_HC_<name>` indices, written
  by hand from `core/src/gen/hostcalls.inc` exactly as `core/include/zb/asset_hostcalls.h` was,
  with the same comment about the generated source of truth
- Create: `tests/host/mock_native_window.h`, `tests/host/native_window_test.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// fromSurface resolves the guest jobject handle through HostJni and hands back a window handle;
// geometry comes from the backend; release drops the handle.
static void check_window(zb::HostNativeWindow& windows, MockNativeWindow& backend, MockJvm& vm) {
    const std::uint32_t surface = vm.new_guest_object_handle("android/view/Surface");
    const std::uint32_t window = call_window(windows, ZB_WINDOW_HC_ANativeWindow_fromSurface,
                                             {kGuestEnv, surface});
    CHECK(window != 0);
    CHECK(call_window(windows, ZB_WINDOW_HC_ANativeWindow_getWidth, {window}) == 1080);
    CHECK(call_window(windows, ZB_WINDOW_HC_ANativeWindow_getHeight, {window}) == 2376);
    call_window(windows, ZB_WINDOW_HC_ANativeWindow_release, {window});
    CHECK(call_window(windows, ZB_WINDOW_HC_ANativeWindow_getWidth, {window}) == -1);  // gone
    CHECK(backend.released() == 1);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build/host native_window_test
```
Expected: FAIL, "zb/host_native_window.h: No such file".

- [ ] **Step 3: Implement**

`NativeWindowBackend` is five virtuals: `from_surface(void* env, void* surface)`, `acquire`,
`release`, `query(void* window, Query)` for width/height/format, and
`set_buffers_geometry(void*, int, int, int)`. `HostNativeWindow` owns a `GlobalHandles` table,
resolves guest `jobject` handles through `HostJni::current_env()` / `resolve_ref()` (the two
methods `HostAssets` already uses), and serves the `ZB_WINDOW_HC_*` indices. Unknown handles
return -1 for queries and are counted as rejections; they never reach the backend.

`ANativeWindow_toSurface` returns a guest `jobject` handle through `HostJni`.

- [ ] **Step 4: Run the test**

```bash
ctest --test-dir build/host -R native_window_test --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/native_window_backend.h core/include/zb/host_native_window.h core/include/zb/window_hostcalls.h core/src/android/host_native_window.cpp tests/host/mock_native_window.h tests/host/native_window_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt; git commit -m "window: ANativeWindow host calls behind a backend seam"
```

---

### Task 6: Chain both into `GuestJniEngine`

**Files:**
- Modify: `core/include/zb/proxy_runtime.h`, `core/src/jni/proxy_runtime.cpp`
- Create: `tests/host/egl_chain_test.cpp`
- Modify: `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`tests/host/egl_chain_test.cpp`, modelled on `tests/host/gl_chain_test.cpp` (two modes, one
process each, because the runtime is process-lifetime):

```cpp
// With backends passed, an egl* and an ANativeWindow_* host call both reach their handler and
// the GL, asset and JNI ranges still work.
CHECK(probe_call("eglGetError") == 0x3000);            // EGL_SUCCESS
CHECK(probe_call("ANativeWindow_getWidth") == -1);     // unknown handle, no crash
CHECK(report.unimplemented_host_calls() == 0);
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build/host egl_chain_test; ctest --test-dir build/host -R egl_chain_test --output-on-failure
```
Expected: FAIL — the engine has no EGL backend parameter yet.

- [ ] **Step 3: Implement the chaining**

`GuestJniEngine` takes `EglBackend*` and `NativeWindowBackend*` (both optional, like
`AssetBackend*`), creates `HostEgl`/`HostNativeWindow` when they are given, and extends the
lambda in `core/src/jni/proxy_runtime.cpp:317`:

```cpp
runtime_->set_host_call_handler([host_jni, host_gl, host_assets, host_egl, host_windows](
                                    std::uint32_t index, GuestThread& thread) {
    if (host_gl != nullptr && host_gl->handle_host_call(index, thread)) return true;
    if (host_assets != nullptr && host_assets->handle_host_call(index, thread)) return true;
    if (host_windows != nullptr && host_windows->handle_host_call(index, thread)) return true;
    if (host_egl != nullptr && host_egl->handle_host_call(index, thread)) return true;
    return host_jni->handle_host_call(index, thread);
});
```

Update the comment above it: the ranges are GLES 0-141, assets 142-159, windows 160-167, EGL
168+, JNI 0xFB00+.

- [ ] **Step 4: Run the whole suite**

```bash
ninja -C build/host; ctest --test-dir build/host --output-on-failure
```
Expected: every test passes, including the existing 37.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/proxy_runtime.h core/src/jni/proxy_runtime.cpp tests/host/egl_chain_test.cpp tests/host/CMakeLists.txt; git commit -m "egl: chain HostEgl and HostNativeWindow into GuestJniEngine"
```

---

### Task 7: Report section and the context-thread check

**Files:**
- Modify: `core/include/zb/runtime_report.h`, `core/src/runtime_report.cpp`,
  `core/src/gl/host_egl.cpp`, `core/src/gl/host_gl.cpp`
- Modify: `tests/host/runtime_report_test.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
report.note_egl_object("context", "config=3 client-version=2");
report.note_egl_current(4242);      // eglMakeCurrent on host tid 4242
report.note_gl_thread(4243);        // a gl* call arrived on another thread
report.note_egl_error("eglCreateWindowSurface", 0x300B);
std::string text = report.text();
CHECK(line_with(text, "egl-context:") == "egl-context: config=3 client-version=2");
CHECK(line_with(text, "egl-thread-mismatch:") == "egl-thread-mismatch: current=4242 gl=4243");
CHECK(line_with(text, "egl-first-error:") == "egl-first-error: eglCreateWindowSurface 0x300b");
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build/host runtime_report_test; ctest --test-dir build/host -R runtime_report_test --output-on-failure
```
Expected: FAIL, "no member named note_egl_object".

- [ ] **Step 3: Implement**

Add the four notes to `RuntimeReport` (same locking and observer pattern as `note_gl_call`), an
`egl-*` block in `text()`, and clear them in `clear()`. `HostEgl` records objects, swaps and the
first error; `HostGl::handle_host_call` reports the host tid of the first `gl*` call after each
`eglMakeCurrent`, and the mismatch line appears only when they differ.

- [ ] **Step 4: Run the tests**

```bash
ctest --test-dir build/host --output-on-failure
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/runtime_report.h core/src/runtime_report.cpp core/src/gl/host_egl.cpp core/src/gl/host_gl.cpp tests/host/runtime_report_test.cpp; git commit -m "report: EGL section and the context-thread mismatch check"
```

---

### Task 8: Android backends

**Files:**
- Create: `core/android/egl_driver_backend.{h,cpp}`,
  `core/android/native_window_driver_backend.{h,cpp}`
- Modify: `core/CMakeLists.txt`, `core/android/guest_jni_runtime.h`

- [ ] **Step 1: Write the driver backends**

`EglDriverBackend` overrides every `EglBackend` virtual with a direct call to the `::egl*` entry
point of the same name, exactly as `core/android/gl_driver_backend.h` does for GLES (its
overrides are generated by the one-off script described in
`docs/superpowers/plans/2026-09-16-phase5-gles.md`, Task 8; reuse it).
`AndroidNativeWindowBackend` calls `ANativeWindow_fromSurface` and friends from
`<android/native_window_jni.h>`.

- [ ] **Step 2: Wire them into the runtime**

In `core/android/guest_jni_runtime.h`, add both members next to `gl_backend_` and pass them to
`GuestJniEngine`.

- [ ] **Step 3: Build for the device**

```bash
ninja -C build/android-arm64 zbridge zbproxy
```
Expected: links. `zbridge` now also links `EGL` (it already links `GLESv2` and `android`).

- [ ] **Step 4: Check the exported symbols are real**

```bash
readelf -d build/android-arm64/core/libzbridge.so | grep NEEDED
```
Expected: `libEGL.so` appears.

- [ ] **Step 5: Commit**

```bash
git add core/android core/CMakeLists.txt; git commit -m "egl: real device backends for EGL and ANativeWindow"
```

---

### Task 9: Guest probe `zbeglprobe`

**Files:**
- Create: `guest/tests/zbeglprobe.c`
- Modify: `tools/build_guest.sh`, `tests/host/egl_chain_test.cpp`,
  `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the probe**

```c
/* Drives the whole EGL sequence through the guest stubs. Printed values are checked by the host
   test, which supplies the mock backends. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>

int main(void) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) return 1;
    EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig config = 0;
    EGLint count = 0;
    if (!eglChooseConfig(display, attribs, &config, 1, &count) || count < 1) return 2;
    EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) return 3;
    printf("egl %d.%d configs=%d\n", major, minor, count);
    return 0;
}
```

- [ ] **Step 2: Build it**

```bash
tools/build_guest.sh; ls -la build/guest/zbeglprobe
```
Expected: the file exists.

- [ ] **Step 3: Drive it from the host test**

Extend `egl_chain_test.cpp` to load the probe through `LibraryRuntime` with the mock backends
wired in, run it, and check its exit status is 0 and its stdout is `egl 1 4 configs=1`.

- [ ] **Step 4: Run**

```bash
ctest --test-dir build/host -R egl_chain_test --output-on-failure; tools/run_guest_tests.sh
```
Expected: PASS, and the existing guest suite still passes.

- [ ] **Step 5: Commit**

```bash
git add guest/tests/zbeglprobe.c tools/build_guest.sh tests/host/egl_chain_test.cpp tests/host/CMakeLists.txt; git commit -m "egl: guest probe for the whole EGL sequence"
```

---

### Task 10: HostLooper state and host fd polling

**Files:**
- Create: `core/include/zb/host_looper.h`
- Create: `core/src/android/host_looper.cpp`
- Create: `tests/host/host_looper_test.cpp`
- Modify: `core/CMakeLists.txt`
- Modify: `tests/host/CMakeLists.txt`
- Modify: `core/include/zb/host_platform_compat.h`
- Modify: `core/src/android/host_platform_compat.cpp`
- Modify: `core/include/zb/proxy_runtime.h`
- Modify: `core/src/jni/proxy_runtime.cpp`
- Modify: `tests/host/platform_compat_test.cpp`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: host-call constants 220-227 from `zb/platform_compat_hostcalls.h`,
  `LibraryRuntime::call_on_current(uint32_t, const GuestCall&)`, and guest memory bounds checks.
- Produces: `HostLooper::HostLooper(LibraryRuntime&)` and
  `bool HostLooper::handle_host_call(uint32_t, GuestThread&)`. `GuestJniEngine::host_looper()`
  always returns the process-lifetime instance.

- [ ] **Step 1: Write the failing host test**

Create `host_looper_test.cpp`. Use a real `LibraryRuntime`, two `GuestThread`s and real
`eventfd()` descriptors. Drive the ABI through registers and a mapped guest stack. Assert these
literal behaviors:

```cpp
CHECK(call(looper, first, ZB_COMPAT_HC_ALooper_forThread) == 0);
const uint32_t a = call(looper, first, ZB_COMPAT_HC_ALooper_prepare, 1);
CHECK(a != 0);
CHECK(call(looper, first, ZB_COMPAT_HC_ALooper_forThread) == a);
CHECK(call(looper, first, ZB_COMPAT_HC_ALooper_prepare, 0) == a);
const uint32_t b = call(looper, second, ZB_COMPAT_HC_ALooper_prepare, 1);
CHECK(b != 0 && b != a);

// No-callback registration: fd, ident=42, INPUT, callback=0, data=0x12345678.
CHECK(add_fd(looper, first, a, fd, 42, 1, 0, 0x12345678) == 1);
CHECK(write(fd, &one, sizeof one) == sizeof one);
CHECK(poll_once(looper, first, 1000, out_fd, out_events, out_data) == 42);
CHECK(guest_u32(out_fd) == static_cast<uint32_t>(fd));
CHECK(guest_u32(out_events) == 1);
CHECK(guest_u32(out_data) == 0x12345678);
CHECK(remove_fd(looper, first, a, fd) == 1);
CHECK(remove_fd(looper, first, a, fd) == 0);

CHECK(poll_once(looper, first, 0, 0, 0, 0) == -3);  // TIMEOUT
CHECK(wake(looper, first, a) == 0);
CHECK(poll_once(looper, first, 0, 0, 0, 0) == -1);  // WAKE
```

Also assert `-1` from `addFd` for an unknown handle, negative fd, a callback-less registration
without `ALLOW_NON_CALLBACKS`, and invalid ident; assert `ALOOPER_POLL_ERROR` for unreadable
non-null output pointers. The production mutations this catches are: a global rather than
per-thread looper, fake `addFd` success, missing readiness, wrong event mapping, and unchecked
guest writes.

- [ ] **Step 2: Run RED**

```bash
ninja -C build/host host_looper_test && build/host/tests/host/host_looper_test
```

Expected: compile failure because `zb/host_looper.h` does not exist. Do not create production
files before observing this failure.

- [ ] **Step 3: Implement the state and non-callback path**

Define focused private records in `HostLooper`:

```cpp
struct Registration {
    int fd;
    int ident;
    int events;
    uint32_t callback;
    uint32_t data;
    uint64_t serial;
};
struct Looper {
    uint32_t handle;
    int options;
    uint32_t references;
    int wake_fd;
    std::unordered_map<int, Registration> registrations;
};
```

Use `eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)` for `wake_fd`. `pollOnce` copies a registration
snapshot under the mutex, unlocks, calls host `poll`, drains the wake eventfd, and maps
`POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL` to ALooper bits 1/2/4/8/16. Before writing no-callback
outputs, validate every non-null address with `GuestMemory::host_ptr(..., 4, kPageWrite)`.
Use `memcpy`, not a cast, for guest writes. A destructor closes only internal wake fds.

Move all eight ALooper indices out of `HostPlatformCompat`; that class returns `false` for them.
Construct `HostLooper` in `GuestJniEngine`, place it before `HostPlatformCompat` in the chain, and
expose `host_looper()` for tests. Do not move or regenerate the indices.

- [ ] **Step 4: Run GREEN and the adjacent tests**

```bash
ninja -C build/host host_looper_test platform_compat_test proxy_runtime_test
build/host/tests/host/host_looper_test
build/host/tests/host/platform_compat_test
build/host/tests/host/proxy_runtime_test
```

Expected: all pass. `platform_compat_test` now expects eight remaining non-looper fallbacks, not
the ALooper bootstrap behavior.

- [ ] **Step 5: Update handoff and commit**

Record the RED/GREEN evidence and the still-missing callback path in `AGENTS.md`.

```bash
git add AGENTS.md core/CMakeLists.txt core/include/zb/host_looper.h \
  core/src/android/host_looper.cpp core/include/zb/host_platform_compat.h \
  core/src/android/host_platform_compat.cpp core/include/zb/proxy_runtime.h \
  core/src/jni/proxy_runtime.cpp tests/host/CMakeLists.txt \
  tests/host/host_looper_test.cpp tests/host/platform_compat_test.cpp
git commit -m "platform: poll guest looper file descriptors"
```

---

### Task 11: Translated guest callback probe

**Files:**
- Create: `guest/testlib/zblooperprobe.c`
- Modify: `tools/build_guest.sh`
- Modify: `tests/host/host_looper_test.cpp`
- Modify: `tests/host/CMakeLists.txt`
- Modify: `core/src/android/host_looper.cpp`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: Task 10's registration snapshot and `LibraryRuntime::call_on_current`.
- Produces: callback registrations invoke `int callback(int fd, int events, void* data)` in
  translated arm32 code; zero removes the exact registration serial and nonzero retains it.

- [ ] **Step 1: Write and build the guest probe before callback dispatch exists**

Create `zblooperprobe.c`, linked against the generated guest `libandroid.so`. Export
`int zb_looper_probe(void)`. It must:

```c
static int calls;
static int callback(int fd, int events, void* data) {
    uint64_t value = 0;
    if (!(events & ALOOPER_EVENT_INPUT)) return 0;
    if (read(fd, &value, sizeof(value)) != sizeof(value)) return 0;
    calls += (int)value + (data == (void*)0x1234);
    return 1;
}
```

The exported probe prepares a looper, creates a nonblocking eventfd, registers the callback,
writes 1, requires `ALooper_pollOnce(1000, NULL, NULL, NULL) == ALOOPER_POLL_CALLBACK`, requires
`calls == 2`, removes the fd, releases the looper and closes only its own fd. Return a distinct
positive line number on each failure and 0 on success.

Add its clang command to `tools/build_guest.sh` with `-Lbuild/guest/lib -landroid`. Extend the
host test with a `guest` mode that starts `GuestJniEngine`, loads `libzblooperprobe.so`, resolves
`zb_looper_probe`, calls it on the service thread, and checks result 0.

- [ ] **Step 2: Run RED**

```bash
tools/build_guest.sh
ctest --test-dir build/host -R host_looper_guest --output-on-failure
```

Expected: the probe reaches polling but fails because Task 10 does not invoke its guest callback.

- [ ] **Step 3: Implement nested callback dispatch**

For every ready callback registration, build this literal call and invoke outside the mutex:

```cpp
GuestCall call;
call.regs = {static_cast<uint32_t>(registration.fd),
             static_cast<uint32_t>(events), registration.data, 0};
const auto result = runtime_.call_on_current(registration.callback, call);
```

Return `ALOOPER_POLL_ERROR` if nested invocation fails. If `result->r0 == 0`, erase only when the
current fd registration still has the snapshot's serial; otherwise preserve a concurrent
replacement. Invoke all callback registrations returned ready by one `poll`, then return
`ALOOPER_POLL_CALLBACK`.

- [ ] **Step 4: Add the callback-removal case and run GREEN**

Extend the guest probe with a second callback that returns zero. After one ready poll,
`ALooper_removeFd` must return 0 because the bridge already removed it. Rebuild and run:

```bash
tools/build_guest.sh
ctest --test-dir build/host -R host_looper --output-on-failure
```

Expected: unit and translated guest modes pass.

- [ ] **Step 5: Update handoff and commit**

```bash
git add AGENTS.md guest/testlib/zblooperprobe.c tools/build_guest.sh \
  tests/host/host_looper_test.cpp tests/host/CMakeLists.txt core/src/android/host_looper.cpp
git commit -m "platform: dispatch guest ALooper callbacks"
```

---

### Task 12: Regression, APK and device acceptance

**Files:**
- Create after a successful device run: `docs/phase7a-acceptance.md`
- Modify: `AGENTS.md`, `CLAUDE.md`

- [ ] **Step 1: Run the full local regression**

```bash
python3 tools/gen_gles.py --check
python3 tools/gen_egl.py --check
python3 tools/gen_jni.py --check
ninja -C build/host
ctest --test-dir build/host --output-on-failure
tools/build_guest.sh
tools/run_guest_tests.sh
ninja -C build/android-arm64 zbridge zbproxy
tools/make_launcher_bundle.sh
```

Expected: all commands pass.

- [ ] **Step 2: Build the APK**

```bash
cd android/launcher
ANDROID_HOME=$HOME/android-sdk ANDROID_SDK_ROOT=$HOME/android-sdk ./gradlew --no-daemon assembleDebug
cd ../..
cp android/launcher/app/build/outputs/apk/debug/app-debug.apk /sdcard/ZettaBridge-debug.apk
sha256sum /sdcard/ZettaBridge-debug.apk
```

- [ ] **Step 3: Ask the user for one run**

The user installs the APK, force-stops the launcher, launches the Flutter guest and sends the
Last run report. Acceptance requires a visible Flutter frame, `guest-exit: (none)`, EGL context
and window surface activity, rising `egl-swaps`, no thread mismatch and no unimplemented host
call. A new first missing function becomes a separately designed follow-up; do not disguise it.

- [ ] **Step 4: Record and commit acceptance**

Write `docs/phase7a-acceptance.md` in the shape of `docs/phase5-acceptance.md`, update state and
gotchas in `CLAUDE.md` and `AGENTS.md`, then run `git diff --check`.

```bash
git add docs/phase7a-acceptance.md AGENTS.md CLAUDE.md
git commit -m "docs: accept the native surface phase"
```
