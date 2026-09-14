# Guest/system boundary: design

Date: 2026-09-13. Status: approved in brainstorming, pending written-spec review.

## Scope

This spec covers how translated arm32 guest code gets its system services: the CPU
translator, guest memory, loading, syscalls, threads, signals, and the host-call
mechanism. It also absorbs most of the former "guest ELF loader" work, because the
real arm32 bionic linker does that job.

Out of scope (each needs its own spec before its phase starts):

- **Part 1, Java <-> native.** Intercepting guest `System.loadLibrary`, guest
  `JNI_OnLoad`, binding `native` methods, the synthesized 32-bit JNIEnv.
- **Part 4, render/audio/assets.** GLES passthrough semantics, OpenAL's AudioTrack
  path (goes through JNI), `AAsset*`.
- The Phase 0 plugin skeleton (DexClassLoader, resources, pinned shortcuts).

## Decisions

1. **The guest runs a real arm32 bionic.** Guest libc, libm, libdl, liblog, libz,
   libc++ and `/system/bin/linker` come from an AOSP arm32 build and run as translated
   guest code. We translate syscalls, not libc functions.
   - Rejected: trampolines into host 64-bit bionic. Every call carrying a struct,
     pointer or `long` would need hand-written 32->64 marshaling (`stat`, `addrinfo`,
     `tm`, `sigaction`, `dirent`, `FILE`, `jmp_buf`, pthread types). `__aeabi_*`,
     unwinder and `setjmp` would need guest-side reimplementation, and each new app
     would bring new functions.
   - Rejected: a Berberis-style generated libc shim. Berberis marshals only between
     same-bitness ABIs, so the 32->64 generator would still be ours to write.
2. **The translator is Dynarmic, the Vita3K fork, at a pinned commit.** It is licensed
   0BSD, is designed as an embeddable library, and has an AArch64 host backend for A32
   guests.
   - Rejected: qemu linux-user. It is GPLv2 and would force GPL on release. It also
     assumes it owns the whole process. qemu stays usable as a developer-only
     reference tool, never shipped.
3. **Host services are reached only through `svc` with a reserved immediate.** Only
   GLES, `AAsset*` and JNI (parts 1 and 4) need host calls. Everything else is a
   syscall.
4. **The guest runs in a separate `:guest` process.** A guest `exit()` or crash ends
   only that process, like a normal app.

## Verified facts this design rests on

Orange Roulette is the first test case, not the product. These facts were checked
against its APK and against upstream sources on 2026-09-13.

- **Build attributes.** All 6 guest libs are `Tag_CPU_arch: v7`. They mix ARM with
  Thumb-1 or Thumb-2, use VFPv2/v3/v3-D16, have no NEON tag, and use the softfp ABI.
- **Import resolution.** There are 443 unique imports. 57 resolve to other guest
  libs. Of the remaining 386, all but 146 are exported by modern bionic
  libc/libm/libdl. The 146 are 128 `gl*`, 11 zlib (satisfied by arm32 system
  `libz.so`), 6 `AAsset*`, and `__android_log_print` (satisfied by arm32 `liblog.so`).
- **Missing bionic symbols.** The three `__cxa_type_match/begin_cleanup/call_unexpected`
  imports are weak. `__aeabi_d2lz` is strong and was not found in modern
  `libc.map.txt`, so it may need a one-function shim library.
- **Linker compatibility: correction found in Phase 2.** Android 16 bionic (LineageOS
  `lineage-23.0`) still basenames full-path `DT_NEEDED` and allows `DT_TEXTREL` when the
  target SDK is below 23. The **Android 17 GSI linker has removed both paths**. Its
  binary has no "invalid DT_NEEDED entry" string, and with target SDK 16 set (verified
  through `android_get_application_target_sdk_version`) it still rejects both. Old
  libraries therefore need import-time fixups; see "Import-time library fixups".
- **ARMv8-only instructions.** The GSI arm32 bionic is built for armv8-a and uses T32
  instructions Dynarmic lacked (`LDA*`/`LDAEX*`/`STL*`/`STLEX*`, `CRC32*`). They are
  added by `third_party/patches/dynarmic-0001-thumb32-armv8.patch`; scudo's malloc
  needs `crc32cw`.
- **Where 32-bit system files come from.** `aosp_arm64.mk` inherits `core_64_bit.mk`
  (64-bit plus 32-bit) on android13, android14, android15 and main. Nobody has yet
  opened an actual image to confirm `/system/lib` is present; see Risks.
- **Dynarmic interface.** `UserCallbacks` provides memory read/write, `CallSVC(swi)`,
  `ExceptionRaised`, and a `Coprocessor` interface (CP15). `UserConfig` provides
  `fastmem_pointer`, `code_cache_size` (default 128 MiB) and `ArchVersion` from v5TE
  to v8. `ExclusiveMonitor(processor_count)` is fixed-size. `Jit` provides `Run`,
  `HaltExecution` and `InvalidateCacheRange`.
- **Dynarmic's AArch64 backend and A32.** The backend has 54 unimplemented IR opcodes,
  but checking actual widths shows none is reachable from the A32 frontend. They are
  all FP16, 64-bit-element vector, or scalar saturation ops used only by A64.
- **Dynarmic's signal handling.** It installs its own SIGSEGV/SIGBUS handler for
  fastmem and chains unhandled faults to the previous handler.
- **ART libsigchain.** It interposes `sigaction`, `signal` and `sigprocmask`. ART's
  special handlers run first; the rest is forwarded to user handlers.
- **bionic threads.** `pthread_create` uses `clone(CLONE_VM|CLONE_FS|CLONE_FILES|
  CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|
  CLONE_CHILD_CLEARTID)`. `gettid()` returns the tid cached in `pthread_internal_t`.
- **OpenAL.** Guest `libopenal.so` outputs audio through Java `android.media.AudioTrack`
  via JNI. No native audio trampolines are needed.
- **Toolchain.** NDK r29 (`~/android-ndk-r29`, `linux-arm64` prebuilt) builds static
  and dynamic armeabi-v7a executables on this aarch64 host.

## Architecture

```
:guest process (separate from launcher UI)
 |
 +-- Java: GuestActivity + DexClassLoader                       (Phase 0, part 1)
 |
 +-- libzbridge.so (arm64)  ==  core/ built for Android
 |     +-- Dynarmic A32 JIT, one instance per executing thread
 |     +-- guest memory: one 4 GiB reservation, fastmem
 |     +-- mini ELF loader: maps zbhost + guest linker, builds initial stack
 |     +-- syscall layer: arm32 EABI syscalls -> host aarch64 kernel
 |     +-- thread manager: guest threads, borrowed carrier threads
 |     +-- signal layer: guest-visible handlers/masks, fault delivery
 |     +-- host-call dispatcher: svc #0x5Axxxx -> generated host handlers
 |
 +-- guest sysroot (arm32, extracted from AOSP GSI aosp_arm64)
 |     system/bin/linker, system/lib/{libc,libm,libdl,liblog,libz,libc++}.so
 |
 +-- our arm32 guest binaries (NDK r29, armeabi-v7a)
       zbhost            service loop: dlopen / dlsym / call / carrier entry
       libGLESv2.so      generated stubs: each function is `svc #imm; bx lr`
       libandroid.so     same, for AAsset*
       libzbcompat.so    shims for symbols absent from modern bionic (__aeabi_d2lz)
```

`core/` has no Android dependency. The same sources build `zbrun`, a Linux aarch64
CLI that runs guest programs without an APK. It is the main development and test
vehicle.

## Memory model

- **Reservation.** One `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` of
  4 GiB plus a 64 KiB guard, anywhere in host address space. `base` is its start and
  becomes Dynarmic's `fastmem_pointer`. A guest address `g` maps to host address
  `base + g`. Because every 32-bit value maps inside the reservation, guest address
  arithmetic cannot escape it. The guard absorbs multi-byte reads at `0xFFFFFFFC`.
- **Guest map.** Guest `mmap`/`munmap`/`mprotect`/`brk` become host calls with
  `MAP_FIXED` inside the reservation, with the same protections. Faults happen
  naturally. The syscall layer keeps its own VMA map and a top-down allocator.
- **Fixed layout:**
  - `0x00000000-0x0000FFFF`: never mapped, so a null dereference faults.
  - Initial guest stack: top `0xFF000000`, 8 MiB.
  - mmap area: top-down below the stack gap.
  - `0xFFFF0000` page: emulated ARM kuser helpers (`__kuser_cmpxchg`,
    `__kuser_get_tls`, `__kuser_memory_barrier`, version word), read+exec. Real
    armeabi v5 code uses them.
- **Cache invalidation.** `munmap`, `mprotect` and `MAP_FIXED` over existing pages
  call `InvalidateCacheRange` on every live JIT. Writes to RWX pages without an
  mprotect are not detected; see Risks.
- **Pointers across the boundary.**
  - Guest to host: add `base` after range-checking.
  - Host to guest: never hand out a host pointer. Copy data into guest memory
    (`glGetString`), or hand out an opaque 32-bit handle (`AAsset*`, and JNI refs in
    part 1).
  - Host 64-bit values that do not fit are reported as `EOVERFLOW`, as the kernel's
    compat layer does.

## Boot and loading

1. **Load.** `zb_init(sysroot_dir, guest_target_sdk)` reserves memory. The mini loader
   maps `zbhost`, and its `PT_INTERP` (`/system/bin/linker`) through path redirection.
2. **Initial stack.** It holds argv, envp and auxv: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
   `AT_ENTRY`, `AT_BASE`, `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP`, `AT_HWCAP2`,
   `AT_SECURE=0`, `AT_EXECFN`. `AT_HWCAP` advertises half, thumb, fastmult, vfp,
   edsp, neon, vfpv3, tls, vfpv4, idiva and idivt, matching Dynarmic `ArchVersion::v8`.
3. **Guest main thread.** A dedicated host thread runs the JIT from the linker entry.
   The linker loads libc, runs constructors, and calls `zbhost` `main()`.
4. **Target SDK.** `zbhost` calls `android_set_application_target_sdk_version(guest_target_sdk)`
   (it lives in `libdl_android.so`), with the value taken from the guest manifest, then
   enters its service loop. This no longer enables pre-M linker behaviour on the Android
   17 sysroot; import-time fixups cover that.
5. **Loading a guest lib.** `zb_dlopen(path)` sends a command to `zbhost`, which runs
   guest `dlopen`. The guest linker resolves system libs from the sysroot and resolves
   `libGLESv2.so`/`libandroid.so` to our stubs. `zb_dlsym` and `zb_call` work the same
   way.
6. **Path redirection.** The syscall layer rewrites these prefixes to `sysroot_dir`:
   - `/system/bin/linker`
   - `/system/lib/`
   - `/apex/com.android.runtime/lib/bionic/`
   - `/linkerconfig/ld.config.txt`, which is ours: a single default namespace
     searching `/system/lib`.

   All other paths pass through unchanged.
7. **Synthesized `/proc` files.**
   - `/proc/cpuinfo` shows the AArch32 compat view an arm64 kernel reports
     (`CPU architecture: 8`), with features matching `AT_HWCAP`.
   - `/proc/self/maps` lists guest mappings only, with 32-bit addresses.
   - `/proc/self/exe` points to `zbhost`.

## Import-time library fixups

When a guest APK is imported, its native libraries are extracted to private storage and
patched by `tools/fix_guest_lib.py`. The launcher will do the same in code. The APK itself
is never modified.

- **Full-path `DT_NEEDED`** (`C:\\Development\\ndk/.../libc.so`). `d_val` is moved to the
  basename tail of the same `.dynstr` string, so no string bytes change.
- **`DT_TEXTREL` / `DF_TEXTREL`.** Replaced by the marker tag `DT_ZB_TEXTREL` (`0x60005A42`,
  OS-specific range). The guest linker ignores it with an "unused DT entry" warning on
  stderr.
  - When a file-backed `mmap2` with `PROT_EXEC` hits a marked file, zbrun maps the
    pages writable inside the emulator and records the range. `mprotect` keeps
    `PROT_WRITE` there.
  - The linker therefore applies text relocations as it did before M, and the guest never
    sees a W+X segment.
- **`__aeabi_d2lz`**, absent from modern libc, comes from `libzbcompat.so`, which is
  preloaded `RTLD_GLOBAL`.

## Transitions

| Direction | Instruction | Handling |
|---|---|---|
| guest -> kernel | `svc #0`, `r7` = syscall number (EABI) | syscall layer |
| guest -> host | `svc #(0x5A0000 \| index)`, index < 0xFFFF | generated host-call handler |
| host -> guest return | `svc #0x5AFFFF` at a fixed "return" address in the kuser page area | ends the current guest call |

- **Stub shape.** Stubs are ARM mode (`svc`, `bx lr`), so Thumb callers interwork. `sp`
  is untouched, which lets handlers read stack arguments at guest `sp` (AAPCS softfp:
  floats in core registers, 64-bit values on an even register pair or 8-byte-aligned
  stack slot). OABI (`svc #0x900000+nr`) is not supported; Android has always been EABI.
- **Host-to-guest call.** Save the JIT registers, put arguments in `r0-r3` and on the
  guest stack, set `lr` to the return address and `pc` to the target (Thumb bit
  honored), `Run()` until the return svc, read `r0`/`r1`, restore registers.
- **Rule: no host work inside Dynarmic callbacks.** `CallSVC` records the request and
  calls `HaltExecution`. The thread's run loop performs the syscall or host call after
  `Run()` returns, then resumes. Host work that re-enters the guest (JNI -> Java ->
  native) therefore runs a nested `Run()` on a JIT that is not currently executing.

## Syscall layer

- **Pass-through.** Integer args are copied as-is; pointer args get `base` added after
  a bounds check. Covers read/write/close, futex, and most fd syscalls.
- **Converted.** `stat64`/`fstat64`/`fstatat64`, `statfs64`, and anything carrying
  `timespec`/`timeval` (32-bit `time_t` on arm32). Also `msghdr`/`iovec`/`cmsghdr`,
  `epoll_event`, `getdents64`, `rlimit`,
  `sysinfo`, `uname` (`machine` reports `armv8l`), and the `ioctl` subset used by
  bionic and the test programs.
- **Emulated, never forwarded to the host.** `sigaction`/`rt_sigaction`,
  `sigprocmask`/`rt_sigprocmask`, `sigaltstack`, `rt_sigreturn`, `signal`-family
  wrappers. See Signals.
- **ARM-private.** `__ARM_NR_set_tls` sets the per-JIT TPIDRURO, which is exposed
  through the CP15 coprocessor; `__ARM_NR_cacheflush` invalidates the JIT cache range.
- **Refused, with a log line.**
  - `fork`, `vfork`, `execve`, `clone` without `CLONE_VM|CLONE_THREAD`: `EPERM`.
  - `ptrace`: `EPERM`.
- **`personality` is emulated per process.** bionic arm32 calls
  `personality(PER_LINUX32)` during init and aborts if it fails, and a 64-bit-only
  kernel refuses it. Found in Phase 1.
- **`exit_group`.** Ends the `:guest` process, or `zbrun`, with the guest status.
- **Unknown syscalls.** Return `ENOSYS` and log the syscall name once.

## Threads

- **Guest-created threads** (`clone` with `CLONE_VM|CLONE_THREAD`). Create a host
  pthread with a new JIT that shares memory and the `ExclusiveMonitor`.
  - Child registers are a copy of the parent's, with `r0=0`, `sp=child_stack` and
    TPIDRURO=`tls`.
  - `CLONE_PARENT_SETTID` and `CLONE_CHILD_CLEARTID` are honored in guest memory,
    including the futex wake on exit.
  - The guest tid is the host tid.
- **Host threads entering the guest** (Java UI thread, `GLThread`). Each one borrows a
  **carrier**.
  - On first entry, `zbhost` runs guest `pthread_create(carrier_entry)`. The carrier
    immediately makes a blocking host call that reports its TLS pointer and current
    `sp`, then parks.
  - The entering host thread gets its own JIT. It runs guest code with TPIDRURO equal
    to the carrier's TLS and a stack below the carrier's parked `sp`.
  - For bionic this is a legitimate thread that happens to execute on a different
    host thread. GL calls therefore run on `GLThread`, where the EGL context is current.
  - When the host thread detaches, the carrier is unparked and returns, and bionic
    cleans up normally.
- **Tid mapping.** A borrower's cached `gettid()` is the carrier's tid, so mutex
  ownership stays consistent. `tgkill`/`tkill` aimed at a carrier tid are redirected
  to the borrowing host thread while it is borrowing.
- **Resources.**
  - `code_cache_size` is 32 MiB per JIT.
  - The `ExclusiveMonitor` is sized for 256 processor ids, allocated from a pool and
    freed on thread exit.
  - Invalidations are broadcast to all JITs.

## Signals

- **Guest dispositions** are stored per guest process, **masks** per guest thread.
  Neither is ever installed on the host.
- **Host handlers.** `zbridge` installs SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGALRM/SIGPIPE
  handlers through `sigaction`. On device, libsigchain places them after ART's special
  handlers.
- **Synchronous guest faults.** A fastmem fault is recovered by Dynarmic and turns into
  a memory callback. The callback finds the address unmapped, records a pending
  SIGSEGV with the fault address, and halts the JIT.
- **Delivery.** At an instruction boundary, if the guest has a handler, the layer
  builds an arm32 `rt_sigframe` (siginfo + ucontext with guest registers) on the guest
  stack, or the alternate stack. Execution enters the handler with `lr` = the guest's
  `sa_restorer`. `rt_sigreturn` restores the registers.
- **No guest handler.** Produce a crash report (next section) and end with the signal
  status.
- **Crash handlers from the guest linker.** Found in Phase 2: the Android linker installs
  debuggerd handlers that fork and exec `crash_dump`. `rt_sigaction` keeps the default
  action for any handler whose address lies in the linker image; application handlers
  are unaffected.
- **Frames.** Implemented in Phase 2. VFP state is saved in `uc_regspace` in zbrun's own
  layout (magic, 64 extension-register words, fpscr).
- **Asynchronous signals** (`alarm`, `raise`, `tgkill`, SIGPIPE). Mark the signal
  pending on the target guest thread and call `HaltExecution` on its JIT. Delivery
  happens as above.

## Errors and diagnostics

Every failure names the thing that failed:

- **Guest crash report.** Signal, guest PC, register dump, and the containing library
  and symbol, obtained through guest `dladdr` via `zbhost` when possible, otherwise via
  the VMA map.
- **Unknown syscall.** Name and number, logged once.
- **Missing host-call index.** Stub library and function name.
- **Output.** Logs go to stderr in `zbrun` and to logcat tag `zbridge` on device.

## Source layout and build

- `core/`: C++20, no Android dependency. Guest memory, mini loader, syscall layer,
  threads, signals, host-call dispatcher, Dynarmic integration.
- `third_party/dynarmic/`: Vita3K/dynarmic at a pinned commit recorded in
  `third_party/README.md`, with its bundled dependencies.
- `cli/zbrun/`: Linux aarch64 CLI over `core/`. System clang + CMake + Ninja.
- `guest/`: arm32 sources built with NDK r29 (`armv7a-linux-androideabi21-clang`):
  `zbhost`, generated stub libraries, `libzbcompat.so`, tests.
- `tools/`: sysroot extraction, stub generator and test runner. Python, ASCII only.
- `sysroot/`: extracted arm32 system files. Generated, not source.
- Android app and `libzbridge.so` packaging arrive with Phase 0 / Phase 3.

## Testing

Tests are small C/C++ programs built with NDK r29 for armeabi-v7a. Each prints `PASS`
lines and exits 0; a shell runner diffs the expected output. A later ladder step never
starts before the earlier one passes.

| Step | Test | Accept |
|---|---|---|
| T1 | `zbrun` runs a hand-assembled arm32 blob (no ELF, no syscalls) | correct return value |
| T2 | static arm32 hello (`-static`) | prints; `write`, `mmap`, `brk`, `exit_group` work |
| T3 | dynamic arm32 hello via guest linker + GSI bionic | `printf`, file I/O, `pthread_create`/`join`, `clock_gettime` |
| T4 | suite: C++ exception across libs, `setjmp`/`longjmp`, guest SIGSEGV handler, LDREX/STREX counter across 8 threads, TEXTREL lib, kuser helper call | all `PASS` |
| T5 | `zbhost` dlopens all 6 Orange Roulette libs with targetSdk 16 | success, no unresolved symbols |
| T6 | T3-T5 inside the `:guest` process on device, next to ART | same results; guest `__android_log_print` visible in logcat |

If `qemu-user` can be installed, T2-T4 binaries are also run under `qemu-arm -strace`
as a reference, to diff syscall traces. It is a developer tool only.

## Phase mapping (replaces CLAUDE.md phases 1-3)

- **Phase 0:** plugin skeleton, unchanged.
- **Phase 1:** `zbrun` + Dynarmic + static arm32. Covers T1-T2.
- **Phase 2:** guest linker + GSI bionic under `zbrun`. Covers T3-T5.
- **Phase 3:** `libzbridge.so` in the `:guest` process on device. Covers T6.
- **Phases 4-6:** JNIEnv, GLES, input/audio/assets, unchanged in intent. Each starts
  with its own spec (part 1, part 4).

## Risks and open questions

- **Sysroot availability.** Not yet confirmed that a published `aosp_arm64` GSI
  contains `/system/lib`, or what filesystem its `system.img` uses. erofs would need
  erofs-utils, which is not in the current apt index. This is the first task of the
  plan. Fallback: an older GSI (Android 13), or an Android emulator arm64 system image.
  - Disk: about 11 GiB free; delete the image after extracting about 10 files.
- **Precise guest state at faults** inside a translated block. Needed for correct
  `ucontext` in guest SIGSEGV handlers. To be prototyped in T4.
- **The guest linker and libc** may probe the Android environment
  (`/dev/__properties__`, linkerconfig, `/apex`). Under `zbrun` on Ubuntu the property
  area may be missing; bionic should degrade to empty properties. Verified in T3.
- **RWX self-modifying code** (for example a guest JIT such as Mono) is not detected.
  Out of scope for now; the later fix is write-protecting translated pages.
- **Building Dynarmic with NDK r29** and its dependencies (fmt, mcl, oaknut,
  robin-map). Precedent: Vita3K Android.
- **`__aeabi_d2lz`** may be absent from the GSI libc. Check in T5 and provide it in
  `libzbcompat.so` if needed.
