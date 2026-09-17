# Phase 7 part 1: native surface (EGL and ANativeWindow)

Status: design approved 2026-09-17. Builds on
`2026-09-13-guest-system-boundary-design.md` (host calls, handles) and
`2026-09-16-gles-assets-design.md` (the GLES passthrough this extends).

## Goal

Let a guest create its own GL context on its own thread and present frames, instead of
borrowing the context Java created for a `GLSurfaceView`.

Two device runs motivate it:
- **Lane Racer** (Unity + prime31): `NativeActivity` needs `ANativeActivity_onCreate`, which in
  turn needs a window.
- **perecup_simulator** (Flutter): guest `dlopen` of `libflutter.so` fails with
  `library "libEGL.so" not found`. Flutter drives its own raster thread, gets a `Surface` from
  Java, and calls `ANativeWindow_fromSurface` plus EGL in native code.

Both need the same foundation, so this part builds only that. `NativeActivity` (lifecycle,
input, `ALooper`, `AConfiguration`) is part 2 on top of it.

## Scope

In:
- guest `libEGL.so`: every `egl*` entry point of EGL 1.4 / 1.5 as generated stubs;
- `ANativeWindow_fromSurface`, `_acquire`, `_release`, `_getWidth`, `_getHeight`, `_getFormat`,
  `_setBuffersGeometry`, `_toSurface`;
- an EGL section in the runtime report.

Out (with reasons):
- **`ANativeWindow_lock` / `_unlockAndPost`.** They hand the caller a pointer into a graphics
  buffer, which lives outside the guest's 4 GiB space. Supporting them means a guest bounce
  buffer and a copy per frame. Only software renderers use them; GL guests do not.
- **GLES 3.x.** The generator already reads `gl.xml`; add it when a guest's report shows the
  calls. Flutter's OpenGL backend and Unity 4/5 run on GLES 2.0.
- **Vulkan.** `libvulkan.so` stays absent; engines fall back to GL when it fails to load.
- **`NativeActivity`, input, `ALooper`, `AConfiguration`.** Part 2.

## Architecture

```
guest libflutter.so
  |  egl*                          ANativeWindow_*
  v                                 v
guest libEGL.so (stubs)          guest libandroid.so (stubs)
  |  svc #0x5A0000|index            |
  v                                 v
HostEgl  <---- handles ---->  HostNativeWindow          (core/, no Android dependency)
  |                                 |
  v                                 v
EglBackend                    NativeWindowBackend       (abstract seam)
  |                                 |
  v                                 v
EglDriverBackend              AndroidNativeWindowBackend (core/android/, real NDK)
```

`GuestJniEngine` chains `HostEgl` and `HostNativeWindow` into the host-call handler next to
`HostGl`, `HostJni` and `HostAssets`, exactly as Phase 5 chained `HostAssets`.

## Components

### Guest stubs

`tools/gen_stubs.py` gains two libraries:
- `libEGL` from the NDK `EGL/egl.h` and `EGL/eglext.h` exported names;
- the existing `libandroid` grows the `ANativeWindow_*` names above.

Indices continue the single stub space in `core/src/gen/hostcalls.inc` (currently 0-159, and
everything below `0xFB00` is free).

### `tools/gen_egl.py`

Mirrors `tools/gen_gles.py`:
- input: `third_party/registry/egl.xml`, pinned from
  `https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/egl.xml`;
- output: `core/include/zb/egl_hostcalls.h`, `core/src/gen/egl_dispatch.inc`,
  `core/include/zb/egl_backend.h`;
- `--check` fails a ctest (`gen_egl_check`) when the committed files are stale.

Mechanical functions are generated whole. Hand-written cases, for the same reasons as in GLES:
`eglChooseConfig`, `eglGetConfigs` (guest array out), `eglQueryString` (host pointer return),
`eglGetProcAddress` (see below), `eglCreateWindowSurface` (window handle), `eglSwapBuffers`,
`eglGetCurrentDisplay/Context/Surface` (handle lookup by host value).

### Handles

`EGLDisplay`, `EGLConfig`, `EGLSurface`, `EGLContext` and `ANativeWindow*` are host pointers.
They cross to the guest only as 32-bit handles from `GlobalHandles`, the table `HostAssets`
already uses. Guest code that compares a handle against `EGL_NO_CONTEXT`, `EGL_NO_SURFACE` or
`EGL_NO_DISPLAY` (all 0) still works, because handle 0 is never allocated. `EGL_DEFAULT_DISPLAY`
(0) passes through to the host as `EGL_DEFAULT_DISPLAY`.

`EGLConfig` values also appear inside guest arrays (`eglChooseConfig`), so those entries are
handles too, allocated when the array is written back.

### `eglGetProcAddress`

It returns a function pointer, which a guest cannot call. It returns the address of the guest
stub for a name we generate, and 0 for everything else. A guest that wants an extension we do
not stub sees "not supported", the same answer a driver without the extension gives.

### Threads

`eglMakeCurrent` binds a context to the calling **host** thread. Guest threads are real host
threads, so an engine with its own render thread works one to one. The dangerous case is a Java
thread that borrows a carrier: the context lands on whichever host thread runs the call. The
design does not try to migrate contexts; `HostEgl` records the host tid of every
`eglMakeCurrent` and of the first `gl*` call after it, and reports a mismatch. That turns a
silent black screen into a line in the report.

### Report

A new EGL section: initialised displays, created contexts and surfaces with their config ids and
sizes, `eglSwapBuffers` count, the first `EGL_BAD_*` error with the function that produced it,
and the thread mismatch above. Rejected calls join the existing `gl-rejections` counter.

## Error handling

The rules are Phase 5's:
- a guest pointer that is null, unreadable or too short fails the call before the driver sees it;
- a failed call sets the EGL error the spec names (`EGL_BAD_PARAMETER`, `EGL_BAD_DISPLAY`,
  `EGL_BAD_SURFACE`, ...) so `eglGetError` answers correctly, returns `EGL_FALSE`, and is
  recorded;
- an unknown handle is `EGL_BAD_*`, never a host pointer dereference;
- nothing aborts the process: a guest that ignores errors must fail the way it would on a real
  device.

## Testing

Host (no device, no driver):
- `egl_marshal_test`: mock `EglBackend`, every generated shape, guest pointer bounds, handle
  allocation and reuse, and that a bad handle never reaches the backend;
- `native_window_test`: mock `NativeWindowBackend`, `ANativeWindow_fromSurface` with a mock JNI
  `jobject` handle, refcounting, geometry;
- `egl_chain_test`: `GuestJniEngine` chains both alongside `HostGl`/`HostJni`/`HostAssets`;
- `gen_egl_check`: committed generated files match the generator.

Guest, on this machine: `zbeglprobe` is an arm32 program that runs the whole sequence
(`eglGetDisplay`, `eglInitialize`, `eglChooseConfig`, `eglCreateContext`,
`eglCreateWindowSurface`, `eglMakeCurrent`, `glClear`, `eglSwapBuffers`). It is driven by a host
test that supplies the mock `EglBackend` and mock window, because this machine has no GL driver,
and it proves the stubs, the marshaling and the handle tables end to end.

Device acceptance: **perecup_simulator (Flutter) draws its first frame** on the OnePlus 13, with
the report showing a created context, a window surface and `eglSwapBuffers` counting up.

## Risks

- **Flutter may need GLES 3.0 anyway** (Impeller). The report will say so: the first
  unimplemented host call names the function. Adding GLES 3 is a generator change.
- **Flutter is heavy.** It starts several threads and a Dart VM. Expect slowness before
  correctness; measure, do not guess.
- **A modern Flutter APK may ship no 32-bit libraries at all**, in which case it is not a test
  case for this phase, only the launcher path is.
- **Carrier threads and contexts** (above) is the most likely source of a silent failure.
