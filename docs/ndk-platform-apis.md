# Step 11: Android NDK platform-call contract

The generated host-call table is append-only. `tools/gen_stubs.py --check` compares committed
outputs with NDK r29 headers and checks the frozen name/library/index mapping for calls 0-401.
The Step 11 event exports append at 402-458 without changing those indices.
`tools/platform_surface.py --json` classifies every generated call; `--elf PATH` additionally
classifies the selected ARM32 ELF's platform imports. APK preflight linkage analysis reports
unresolved imports that have no guest stub. Runtime reports record calls routed to an explicit
failure handler and dynamic-loader symbol failures.

At the Step 11 review, the 459 generated calls classify as 150 implemented platform calls,
12 explicit failures, and 297 GLES/EGL calls pending the Step 12 correctness audit. A pending
call can have a generated handler; the label does not claim its pointer/error/context contract
has passed the Step 12 audit. These counts refer to the Android build with its platform backends
installed. `--elf` gives the per-guest used subset; no arbitrary APK was audited here.

## Supported contracts

| API | Guest representation and lifetime | Failure and thread behavior |
| --- | --- | --- |
| `AAsset*`, `AAssetDir*` (142-159) | Generation-checked 32-bit handles. `AAsset_getBuffer` and directory names are copies allocated in guest memory; the former lives until asset close, the latter until the next name or directory close. Both are freed on close. File descriptors returned by the NDK are real process FDs owned by the guest. | Strings are bounded to 4096 bytes. Read/output ranges are checked before the backend call. 32-bit lengths, seeks, and descriptor offsets reject overflow; 64-bit results follow AAPCS32 register alignment. `getBuffer` has a 64 MiB per-process staging budget and returns null on failure. Invalid handles return null or -1 as appropriate. |
| `ANativeWindow_*` (160-167, 212-213) | Generation-checked 32-bit handles with NDK acquire/release counts. `lock` copies supported CPU formats into a bounded guest buffer and `unlockAndPost` copies back and frees it. `toSurface` uses the NDK to create a new Java local `Surface`, avoiding a stale borrowed reference. | Queries reject invalid handles. Lock checks the 44-byte ARM32 output struct, optional 16-byte dirty rect, row geometry, format, and a 64 MiB cap. Duplicate locks fail with `-EBUSY`; unlock on the wrong thread fails with `-EPERM`; invalid pointers fail with `-EFAULT`. Unsupported CPU formats fail with `-ENOTSUP`. A locked window must be unlocked before its last release. |
| `ALooper_*` (220-227) | Existing per-guest-thread looper handles. Guest threads poll their registered FDs and dispatch guest callbacks on the polling thread. A Java borrower registers on that host thread's real Android looper and re-enters guest code from the host callback. | Existing host and guest callback tests cover wake, timeout, callback return/unregistration, invalid handles, and borrower dispatch. The real attached callback path is source-implemented and host-mock-tested; this step does not claim a new device callback probe. |
| `AndroidBitmap_getInfo/lockPixels/unlockPixels` (217-219) | JNI object identity is checked with `IsSameObject`; a global reference retains each locked Bitmap. CPU pixels are copied to a bounded guest allocation and copied back on same-thread unlock, then freed and the global reference released. | Output pointers and `stride * height` are checked; supported formats are RGBA_8888, RGB_565, RGBA_4444, A_8, RGBA_F16, and RGBA_1010102. Hardware bitmaps and buffers over 64 MiB fail with `BAD_PARAMETER`; allocation failure returns `ALLOCATION_FAILED`. Duplicate locks, invalid objects and wrong-thread unlocks fail. |
| `AConfiguration_*` (344-394) | Generation-checked 32-bit handles own host `AConfiguration*`. Copy and compare resolve two handles. The two-byte language/country fields are copied through checked guest pointers. `fromAssetManager` resolves an asset-manager handle on the host. | Missing version-specific host symbols are reported and fail explicitly. Integer getters return `-ENOSYS` when unavailable. Invalid handles and pointers are rejected. The Android backend uses the real NDK configuration object under the installed app's UID. |
| `AInputQueue_*` (395-401) | `fromJava` maps an API 33+ Java `InputQueue` to a generation-checked 32-bit queue handle. The Android backend retains its Java owner and uses an Android looper worker to signal a process `eventfd` when input is ready. `attachLooper` registers that fd with the existing guest or Java-borrower looper; callback and ident registrations use the same dispatch contract as `ALooper_addFd`. `getEvent` creates a one-use event handle, never a native pointer. | The Java queue must remain undisposed while in use, as required by the NDK. Queue handles persist for the process because the NDK has no release function; at most 64 distinct queues are retained. Missing `fromJava` on API 29-32 is reported. Invalid queue/event handles, wrong queue ownership and wrong-thread event use fail. `getEvent` checks its four-byte output pointer before consuming an event. `preDispatchEvent` invalidates a consumed/requeued event handle when it returns nonzero. `finishEvent` invalidates it after reporting handled status. Detach removes the guest looper registration; the backend retains its readiness monitoring until process exit. |
| `AInputEvent_*`, `AKeyEvent_*`, `AMotionEvent_*` (402-458) | Key/motion scalar accessors resolve only live event handles from `getEvent`. Integer, 64-bit and float values are copied into the ARM32 AAPCS32 result registers; 64-bit host event pointers stay behind the handle table. | Accessors reject the wrong event type or thread. Pointer and history indices are checked against NDK counts before calling the host accessor; invalid float indices return NaN, integer indices return `-EINVAL`. Missing version-specific symbols are reported. `AInputEvent_release/toJava` and key/motion `fromJava` have separate ownership contracts and currently fail explicitly with reports. |

`AAssetManager_fromJava` retains a Java global reference per distinct native manager for the
process lifetime, because the NDK has no manager close API and its pointer depends on the Java
owner. All 64-bit host pointers stay behind handle tables or backend calls; no pointer value is
returned to the ARM32 guest.

## Explicit failures and current boundaries

- `AInputEvent_release/toJava`, `AKeyEvent_fromJava`, and `AMotionEvent_fromJava` are
  exported but explicitly reported as unsupported. They create or release events outside
  `AInputQueue_getEvent` ownership and need a separate JNI lifetime contract.
- `eglCreateImageKHR`, `eglDestroyImageKHR`, and `glEGLImageTargetTexture2DOES` (214-216)
  retain explicit failure/report behavior. Core `eglCreateImage`, `eglCreateSync`, client-buffer
  and pixmap creation are rejected with EGL errors. These and all GLES/EGL generated calls
  need the Step 12 correctness audit. Core platform window surfaces already resolve guest
  window handles to the NDK window, with invalid handles rejected.
- No sensor, vibration, clipboard, storage/URI, or permission native import was found in the
  18 local synthetic `libzb*.so` probe ELFs (15 distinct platform imports). Java APIs run under
  the installed package's ordinary UID;
  this step adds no privileged bypass. An ungenerated native API remains unresolved and is
  reported by preflight/linker diagnostics. Media, sensor, and input service expansion belongs
  to the later service step, based on a concrete import and permission contract.

## Validation

- AArch64/QEMU: all nine focused Step 11 CTest cases passed after the final pointer and input
  edits. The new input test covers queue notification through guest `pollOnce`, Java-borrower
  callback delivery and unregister, invalid handles/pointers, event ownership, pre-dispatch,
  64-bit accessor returns and wrong-thread access. The full host suite passed 51/52; the
  separate `library_runtime_test` still fails its `RTLD_NOLOAD` lookup for
  `libzbcallprobe.so` in this recovered sysroot/QEMU environment.
- Android arm64: `zbridge`, `zbproxy` and `zbjni_reflection_compile_test` linked. The generated
  ARM32 stubs and both Step 11 probe APKs built. The converter signed and verified the
  installed platform probe APK.
- Pixel 11 Pro XL, Android API 37, enforcing SELinux: the converted Step 11 APK, assigned UID
  10392, reported `0x1f` for asset read, bitmap lock/unlock, configuration, and native-window
  lock/post paths. Java observed the changed bitmap pixel `0xff996633`. Its runtime report
  recorded one guest native call, zero proxy failures and zero unimplemented host calls.
  The direct ARM64 `NativeActivity` test fixture received two separate key down/up pairs
  from `adb input keyevent 66` and `67` through the Android input backend, proving queue
  rearming; each event had type 1 and actions 0/1 with its expected code. A Java-created
  motion event gave native type 2, one pointer and coordinates (12.5, 7.5) through the same
  accessor backend. This is device proof for the NDK queue producer and scalar accessors;
  ARM32 handle and callback marshaling was exercised under QEMU with a mock backend. The
  installed platform probe requested no permissions.

The behavior above follows the pinned NDK headers and the Android NDK [asset](https://developer.android.com/ndk/reference/group/asset),
[native window](https://developer.android.com/ndk/reference/group/a-native-window),
[bitmap](https://developer.android.com/ndk/reference/group/bitmap),
[configuration](https://developer.android.com/ndk/reference/group/configuration), and
[input](https://developer.android.com/ndk/reference/group/input) references. Host/QEMU tests and
Android linking prove marshaling and build contracts, not broad device or arbitrary APK behavior.
