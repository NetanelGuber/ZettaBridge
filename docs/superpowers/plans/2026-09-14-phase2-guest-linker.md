# Phase 2: guest linker + GSI bionic under zbrun (T3-T5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dynamic arm32 executables run under `zbrun` through the real arm32 bionic linker
from the GSI sysroot. Threads and guest signal handlers work, and all 6 Orange
Roulette libs `dlopen` with targetSdk 16.

**Architecture:** Spec `docs/superpowers/specs/2026-09-13-guest-system-boundary-design.md`,
sections "Boot and loading", "Syscall layer", "Threads" and "Signals". Builds on the
Phase 1 core (`Process`, `GuestThread`, syscall layer).

**Tech Stack:** as Phase 1; sysroot from `tools/extract_sysroot.sh`.

**Execution note:** Inline, unattended, discovery-driven, same agent. Each task states
its files, behavior, test and acceptance. Code is written directly during execution,
and anything discovered is recorded in the spec.

**Verified inputs (2026-09-14):**
- **Sysroot contents.** The GSI system.img is ext4. `/system/lib/{libc,libm,libdl}.so`
  are symlinks into the runtime APEX; the regular-file copies are in
  `/system/lib/bootstrap` and `/system/bin/bootstrap/linker`. `libc.so` needs
  `ld-android.so`. `libstdc++.so` is a regular file (needed by the guest `libopenal.so`).
- **Linker startup** (`linker_main.cpp`):
  - `stat("/proc/self/exe")`, then `realpath` for the executable path;
  - `AT_PHDR`/`AT_PHNUM`/`AT_ENTRY` from auxv;
  - `LD_LIBRARY_PATH` honored when `AT_SECURE` is 0;
  - without a readable config, runs a default configuration searching `/system/lib`,
    `/odm/lib`, `/vendor/lib`.
- **Target SDK setter.** `android_set_application_target_sdk_version` lives in
  `libdl_android.so` (version `LIBDL_ANDROID`), not in `libdl.so`.
- **No `__aeabi_d2lz`** in GSI libc.
- **qemu-arm cannot run the guest linker either**, because `personality(PER_LINUX32)`
  fails. There is no reference trace past bionic init.

---

### Task A: path translation + /proc/self/exe

- **Files:** `core/include/zb/process.h`, `core/src/process.cpp`,
  `core/src/syscalls.cpp`, `cli/zbrun/main.cpp`.
- **Behavior:**
  - `Process::translate_path` maps absolute guest paths into `--sysroot` (or
    `ZB_SYSROOT`): `/apex/com.android.runtime/lib/bionic/` -> `/system/lib/`,
    `/apex/com.android.runtime/bin/` -> `/system/bin/`, and `/system/`, `/vendor/`,
    `/odm/`, `/product/`, `/system_ext/`, `/apex/`, `/linkerconfig/` -> the same path
    under the sysroot.
  - `/proc/self/exe` resolves to the guest executable's host path, and `readlinkat` of
    it returns that path.
  - Applied to openat, faccessat, faccessat2, fstatat64, statx, readlinkat, unlinkat,
    mkdirat.
- **Test:** covered by Task C (the linker opens its libs through these paths).

### Task B: PT_INTERP loading

- **Files:** `core/src/process.cpp`.
- **Behavior:**
  - A PIE main executable loads below 0x40000000.
  - If PT_INTERP is present, translate it and load it as ET_DYN below `kMmapLimit`;
    `AT_BASE` = interpreter bias, start PC = interpreter entry, `AT_ENTRY` stays the
    program entry.
- **Test:** covered by Task C.

### Task C: T3a dynamic hello

- **Files:** `guest/tests/hello_dynamic.c`, `guest/tests/expected/hello_dynamic.out`,
  `tools/build_guest.sh` (`*_dynamic.c` built without `-static`),
  `tools/run_guest_tests.sh` (passes `--sysroot`).
- **Accept:** `PASS hello_dynamic`, covering printf, fopen/fgets/unlink,
  `dlopen("libm.so")` + `dlsym("cos")`, `clock_gettime` REALTIME, and malloc.

### Task D: stub libraries + libzbcompat + T5

- **Files:**
  - `tools/gen_stubs.py`: reads a function-name list and emits ARM assembly, one
    global function per name: `svc #(0x5A0000 | index)`; `bx lr`. It also emits the
    host name table `core/src/gen/hostcalls.inc`.
  - `guest/stubs/GLESv2.list`, `guest/stubs/android.list`: names from NDK
    `GLES2/gl2.h` and `android/asset_manager*.h`; the generator parses the headers.
  - `guest/compat/zbcompat.c`: `__aeabi_d2lz`.
  - `tools/build_guest.sh`: builds `libGLESv2.so`, `libandroid.so`, `libzbcompat.so`
    into `build/guest/lib/`.
  - `guest/tests/or_dlopen_dynamic.c`: dlopens `libdl_android.so`; calls
    `android_set_application_target_sdk_version(16)`; `dlopen(RTLD_NOW)` of the six
    libs from a directory argument in the order `std`, `regexp`, `zlib`, `openal`,
    `lime`, `ApplicationMain` (after preloading `libzbcompat.so` with `RTLD_GLOBAL`);
    prints `dlopen <lib>=PASS|FAIL <dlerror>`; then `dlsym` of `JNI_OnLoad` in
    liblime and `Java_org_haxe_HXCPP_main` in ApplicationMain.
  - `tools/run_guest_tests.sh`: extracts `lib/armeabi/*.so` from the APK into
    `build/or/` and runs with `LD_LIBRARY_PATH=build/guest/lib`.
- **Host side:** `svc #0x5Axxxx` outside a host call logs `host call <lib>:<name> not
  implemented` once and returns 0.
- **Accept:** all six `dlopen` PASS and both `dlsym` PASS. This is acceptance T5.

### Task E: threads (clone) + exit semantics

- **Files:** `core/include/zb/process.h`, `core/src/process.cpp`,
  `core/src/syscalls.cpp`, `core/include/zb/guest_thread.h`.
- **Behavior:**
  - `clone` with `CLONE_VM|CLONE_THREAD` starts a host `std::thread` running its own
    `GuestThread` (processor id from a pool of 256).
  - Child registers copy the parent's, with r0=0, sp=child_stack and TLS from
    `CLONE_SETTLS`.
  - `CLONE_PARENT_SETTID` writes the host tid into guest memory;
    `CLONE_CHILD_CLEARTID` on thread exit writes 0 and does a futex wake.
  - `exit` ends only the calling thread. `exit_group` flushes and calls `_exit` when
    other threads exist.
  - `invalidate` is broadcast to all threads under a mutex.
  - Memory syscalls are serialized by a process mutex.
- **Test:** `guest/tests/threads_dynamic.c`. 8 pthreads each increment an
  `atomic_int` 100000 times (LDREX/STREX), plus a mutex-protected counter; join;
  `pthread_self` differs per thread; TLS via `__thread` variable.
  Expected `atomic=PASS mutex=PASS tls=PASS`.
- **Accept:** `PASS threads_dynamic` (T3 threads part).

### Task F: guest signal delivery

- **Files:** `core/src/signals.cpp`, `core/include/zb/signals.h`,
  `core/src/process.cpp`, `core/src/syscalls.cpp`, `tools/abi_check.c` (sigcontext
  and ucontext offsets).
- **Behavior:**
  - A pending signal on a thread with a guest handler builds an arm `rt_sigframe`
    (siginfo 128 bytes + ucontext with sigcontext r0-r15, cpsr, fault_address and
    sigmask) on the guest stack, or on the altstack when `SA_ONSTACK` is set and
    enabled.
  - Entry sets r0=sig, r1=&siginfo, r2=&ucontext, lr=`sa_restorer` (bionic always
    sets `SA_RESTORER`), and pc=handler with the Thumb bit mapped to CPSR.T.
  - The mask becomes `sa_mask | sig` unless `SA_NODEFER`; `SA_RESETHAND` is honored.
  - `rt_sigreturn` restores registers and mask from the frame at sp.
  - A memory fault becomes SIGSEGV with `si_addr`; an undefined-instruction exception
    becomes SIGILL.
  - Self-directed kill/tgkill/rt_tgsigqueueinfo become pending signals instead of
    exiting.
- **Test:** `guest/tests/signals_dynamic.c`:
  - SIGSEGV handler on a null write + `siglongjmp` out;
  - `raise(SIGUSR1)` handler runs;
  - `sigaltstack` + `SA_ONSTACK` handler observes its stack address inside the altstack;
  - `abort()` with the default action exits 134.
- **Accept:** `PASS signals_dynamic`.

### Task G: rest of the T4 suite

- **Files:** `guest/tests/cxx_dynamic.cpp` + `guest/tests/libcxxthrow.so` (built with
  `c++_shared`, shipped from the NDK into `build/guest/lib`), `guest/tests/kuser_dynamic.c`,
  `core/src/process.cpp` (kuser page).
- **Behavior:**
  - Map the kuser helper page at `0xFFFF0000` (read+exec) with ARM code for
    `__kuser_cmpxchg` (0xFFFF0FC0), `__kuser_memory_barrier` (0xFFFF0FA0),
    `__kuser_get_tls` (0xFFFF0FE0; its svc-free version reads TPIDRURO) and
    `__kuser_helper_version` (0xFFFF0FFC = 2).
  - cmpxchg is implemented with LDREX/STREX so the ExclusiveMonitor serializes it.
- **Tests:** a C++ exception thrown in the lib and caught in the executable; the kuser
  version is >= 2; cmpxchg succeeds, then fails on a stale value; `get_tls` equals the
  `mrc` result.
- **Accept:** `PASS cxx_dynamic`, `PASS kuser_dynamic`. With Tasks E-F this completes T4.
