# Phase 7 part 1: native surface (EGL and ANativeWindow)

> **Provenance:** This is an inherited upstream design. Its OnePlus 13 device acceptance
> reference belongs to the original project and is not validation by this fork.

Status: design approved 2026-09-17; Flutter ALooper extension approved 2026-09-17. Builds on
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

Both need the same foundation, so this part builds that first. The device acceptance run then
proved that this Flutter engine also requires the callback-based subset of `ALooper`; that
observed subset is included below. `NativeActivity` lifecycle, input queues and
`AConfiguration` remain part 2 on top of it.

## Scope

In:
- guest `libEGL.so`: every `egl*` entry point of EGL 1.4 / 1.5 as generated stubs;
- `ANativeWindow_fromSurface`, `_acquire`, `_release`, `_getWidth`, `_getHeight`, `_getFormat`,
  `_setBuffersGeometry`, `_toSurface`;
- the `ALooper` subset exercised by Flutter: per-thread prepare/lookup and references, callback
  fd registration/removal, polling and wake;
- an EGL section in the runtime report.

Out (with reasons):
- **`ANativeWindow_lock` / `_unlockAndPost`.** They hand the caller a pointer into a graphics
  buffer, which lives outside the guest's 4 GiB space. Supporting them means a guest bounce
  buffer and a copy per frame. Only software renderers use them; GL guests do not.
- **GLES 3.x.** The generator already reads `gl.xml`; add it when a guest's report shows the
  calls. Flutter's OpenGL backend and Unity 4/5 run on GLES 2.0.
- **Vulkan.** `libvulkan.so` stays absent; engines fall back to GL when it fails to load.
- **`NativeActivity`, input queues, and `AConfiguration`.** Part 2. Its broader looper use will
  reuse the callback looper built here and extend it only if a device trace requires more.

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

guest libandroid.so ALooper stubs
  |
  v
HostLooper -- host poll/eventfd -- nested LibraryRuntime::call_on_current
```

`GuestJniEngine` chains `HostEgl` and `HostNativeWindow` into the host-call handler next to
`HostGl`, `HostJni` and `HostAssets`, exactly as Phase 5 chained `HostAssets`.

### Flutter callback looper extension

The first loader-compatible device run returned null from `ALooper_prepare`, and Flutter
aborted. A per-guest-thread opaque looper fixed that. The next run reached exactly one
unimplemented call, `ALooper_addFd`, then aborted because the safe fallback returned failure.
Disassembly of the shipped arm32 `libflutter.so` establishes the complete required flow:

1. Flutter creates an `eventfd` through the ordinary guest syscall path. Guest descriptors are
   real descriptors in this host process, so no fd translation is required.
2. It registers the fd with `ident = ALOOPER_POLL_CALLBACK`,
   `events = ALOOPER_EVENT_INPUT`, a guest callback address, and guest data; it requires
   `ALooper_addFd` to return 1.
3. Its platform thread loops in `ALooper_pollOnce(-1, NULL, NULL, NULL)`.
4. The callback reads eight bytes from the eventfd, dispatches Flutter work, and returns 1 to
   remain registered.

Move the looper behavior out of the loader-fallback table into a focused `HostLooper` owned by
`GuestJniEngine`. `HostLooper` keeps one looper state per `GuestThread`, represented to the guest
by a stable nonzero 32-bit handle. State contains the prepare options, reference count, an
internal host `eventfd` for wakeups, and registrations keyed by fd. A mutex protects state, but
it is never held while blocking in `poll` or while calling guest code.

`ALooper_addFd` validates the handle, fd, ident/callback combination and event mask, replaces an
existing registration for the same fd, wakes a concurrent poll, and returns 1. `removeFd`
returns 1 only for an existing registration. `wake` signals the internal eventfd.
`pollOnce` snapshots the registrations, polls them together with the internal wake fd, maps host
`poll` bits to `ALOOPER_EVENT_*`, and invokes callback registrations through the existing nested
`LibraryRuntime::call_on_current` path using `(fd, events, data)`. A callback result of zero
removes that exact registration. Wake returns `ALOOPER_POLL_WAKE`; timeout returns
`ALOOPER_POLL_TIMEOUT`; a completed callback returns `ALOOPER_POLL_CALLBACK`.

Non-callback registrations are supported when the looper was prepared with
`ALOOPER_PREPARE_ALLOW_NON_CALLBACKS`: `pollOnce` writes `outFd`, `outEvents` and `outData` only
after validating their guest addresses, then returns the registration's nonnegative ident.
Invalid handles, arguments, output pointers, host poll failures or failed nested callbacks
return `ALOOPER_POLL_ERROR` without aborting the process. Descriptors belong to the guest;
`HostLooper` never closes registered fds. It closes only its own internal wake fds when the
engine ends.

## Components

### Guest stubs

`tools/gen_stubs.py` gains two libraries:
- `libEGL` from the NDK `EGL/egl.h` and `EGL/eglext.h` exported names;
- the existing `libandroid` grows the `ANativeWindow_*` names above.

Indices continue the single append-only stub space in `core/src/gen/hostcalls.inc`. The ALooper
names already occupy compatibility indices 220-227; implementing them does not renumber any
stub, and everything below `0xFB00` after the current last entry remains free.

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
- `host_looper_test`: prepare/lookup isolation, add/replace/remove, real pipe readiness,
  non-callback outputs, wake and timeout behavior, and invalid guest pointers;
- a translated callback probe: register a real guest callback on an eventfd, wake it, run
  `pollOnce`, and prove that the nested callback consumes the event and returns
  `ALOOPER_POLL_CALLBACK`.

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
- **Nested looper callbacks.** `pollOnce` re-enters guest code from a host-call handler. This is
  the same supported nested-call path used elsewhere, but the translated callback probe must
  cover callback return zero, reentrancy and exact registration removal before the device run.
- **Thread lifetime.** The first implementation keeps looper state for the process lifetime,
  matching `GuestJniEngine`. Flutter's platform thread is long-lived. Part 2 must add explicit
  state retirement if NativeActivity testing produces short-lived looper threads.
