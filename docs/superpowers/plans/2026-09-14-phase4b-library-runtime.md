# Phase 4b: Library Runtime, Host-to-Guest Calls, and Carriers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Phase 1-3 executable runner into a reusable, process-lifetime
library-mode guest process that loads arm32 shared libraries, resolves symbols, and calls
them on the service guest thread or on a carrier borrowed by the current host thread.

**Architecture:** The guest `zbhost` enters one reserved host call (`READY`) and serves
host requests from inside it. Foreign host threads borrow the TLS, stack, guest tid and
emulated signal state of parked guest pthreads (carriers) while running their own JIT.
Threads waiting inside a host call park on a per-thread futex word that signal posting
wakes, so asynchronous signals are still delivered. Nested guest calls reuse the existing
syscall, host-call, fault and signal dispatcher.

**Tech Stack:** C++20, C11, Dynarmic A32, arm32 bionic/NDK r29, pthreads, futex,
CMake/Ninja, host tests and guest integration tests.

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md` (part 1), building on
`docs/superpowers/specs/2026-09-13-guest-system-boundary-design.md` (part 3, sections
"Transitions" and "Threads").

## Global constraints

- Guest Java stays on the real 64-bit ART; only guest native code is translated.
- Guest libraries use the real arm32 bionic linker; host code never loads them.
- Host work never runs inside a Dynarmic callback.
- JNI native entry is AAPCS32 base/softfp, including hard-float-built guest libraries.
- Guest pointers are offsets inside the 4 GiB reservation; host pointers never cross.
- Guest `dlopen` flags are 32-bit bionic values (`ZB_GUEST_RTLD_*`); host `<dlfcn.h>`
  constants are never passed to the guest.
- A host thread borrowing a carrier keeps that carrier's TLS, stack, guest tid, signal
  mask, alternate signal stack and FPSCR.
- `svc #0x5AFFFF` is reserved exclusively for an active host-to-guest return whose `sp`
  matches the call frame. Host-call indices `0xFE00-0xFEFF` belong to the runtime.
- The runtime is process-lifetime; there is no shutdown path.
- Code and scripts are English ASCII only; commits are local English imperative commits.
- The existing dirty `third_party/dynarmic` worktree state is never staged.

---

## Scope

This plan implements only the execution substrate needed by JNI. It does not create a
guest `JNIEnv`, call ART, bind native methods, or change the launcher. Those belong to
plans 4c and 4d, which consume the hooks added here (`set_host_call_handler`,
`call_on_current`, `memory`, `service_api`, the zbhost preload slot).

## Review decisions folded into this plan

The review of the first plan and of Tasks 1-3 produced these decisions. Tasks 1-3 were
fixed in commit `a15b86c`; Tasks 4-6 below implement the rest. The Task 4 and Task 5
code was prototyped in the worktree, built, and tested before being written here
(`library_runtime_test` passed 50 of 50 repeated runs; the Android arm64 `zbridge` and
`zbrun` targets linked).

| Id | Decision | Where |
|---|---|---|
| D1 | Guest RTLD constants (`ZB_GUEST_RTLD_NOW` 0, `LAZY` 1, `GLOBAL` 2, `NOLOAD` 4, `NODELETE` 0x1000, `DEFAULT` 0xffffffff); loader APIs take explicit guest flags | Task 4 |
| D2 | A call clears ITSTATE and the E bit on entry | Task 1 (done) |
| D3 | `call_depth`; thread exit inside a call ends the host process; shared `after_stop` | Tasks 1-2 (done) |
| D4 | Stray return svc and return with a mismatched `sp` are illegal instructions | Tasks 1-2 (done) |
| D5 | Process-lifetime runtime (destructor aborts after start); `guest_thread_count`; tests end with `_Exit`; `check.h` uses `_Exit` | Task 2 (check.h, done), Tasks 4-5 |
| D6 | Futex parking woken by `post_signal`; `ZB_SERVICE_AGAIN` loops in zbhost | Task 4 (service), Task 5 (carriers) |
| D7 | Borrower inherits sigmask, altstack, FPSCR; release copies mask and altstack back | Task 5 |
| D8 | Hooks: `set_host_call_handler`, `memory`, `call_on_current`, `service_api`, `guest_thread_count`; indices 0xFE00-0xFEFF reserved in `gen_stubs.py` | Task 4 |
| D9 | `Carrier::load_library` / `find_symbol` with a per-carrier guest `malloc` buffer | Task 5 |
| D10 | Service-thread and nested-borrow misuse guards | Tasks 4-5 |
| D11 | zbhost preload slot; failed preload exits 4; `start` READY timeout | Task 4 |
| D12 | Code cache size parameter (done); carrier JITs 2 MiB; latency report | Task 1 (done), Task 5 |
| D13 | Required tests | Acceptance |
| D14 | Minor plan and diagnostics fixes (fbits type, class order, crash-report guest tid, PI futex note, one-line commands, local commits) | throughout |

## File structure

| File | Responsibility |
|---|---|
| `core/include/zb/guest_thread.h`, `core/src/guest_thread.cpp` | nested call frame, `call_depth`, code cache size, futex parking |
| `core/include/zb/process.h`, `core/src/process.cpp`, `core/src/signals.cpp` | reusable stop dispatch, host-call hook, borrowed JIT registry, `current_thread` |
| `core/include/zb/library_protocol.h` | C-compatible `zbhost` handshake ABI and guest RTLD constants |
| `guest/zbhost/zbhost.c` | guest service entry, preloads, carrier creation |
| `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp` | service queue, loader, carrier leases, hooks |
| `guest/testlib/zbcallprobe.c` | base AAPCS probe shared library |
| `tests/host/guest_call_test.cpp` | nested call state, Thumb/IT entry, signals, exit and stray returns |
| `tests/host/host_call_dispatch_test.cpp` | Process host-call dispatch, borrower inheritance and copy-back |
| `tests/host/zbhost_protocol_test.cpp` | zbhost handshake and preload failure |
| `tests/host/library_runtime_test.cpp` | real linker, loader, signals, carriers, concurrency |
| `tools/build_guest.sh`, `tools/gen_stubs.py`, `core/CMakeLists.txt`, `tests/host/CMakeLists.txt` | build wiring and index reservation |

Commands are one line and run from the repository root of the Phase 4b worktree
(`/home/Zailox/ZettaBridge/.worktrees/phase4b-library-runtime`). Every task ends with:

```sh
tools/build_guest.sh; ninja -C build/host; ctest --test-dir build/host --output-on-failure
```

Edit files with the editor tools or Python `str.replace()` heredocs, never `sed`. Commit
locally after every task; pushing is done together with the user.

---

## Task 1: Nested host-to-guest call frame

**Status:** done. Commits `d0708d8` (call frame) and `a15b86c` (review fixes D2, D3, D4,
D12, D14).

**Files:**
- Modify: `core/include/zb/guest_thread.h`
- Modify: `core/src/guest_thread.cpp`
- Modify: `core/src/process.cpp` (kuser page return svc)
- Create: `tests/host/guest_call_test.cpp`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:** `GuestThread::call`, `GuestThread::call_depth`, `GuestResult`,
`GuestStopHandler`, `kHostReturnSwi`, `kHostReturnAddress`, `kDefaultCodeCacheSize`,
`kCarrierCodeCacheSize`, the `code_cache_size` constructor parameter.

- [x] **Step 1: Add the failing host test**

`tests/host/guest_call_test.cpp` maps tiny ARM and Thumb functions and a writable stack.
Its thread-level part (`thread_level_tests`) checks:

- a function that adds `r0`, `r1` and its first stack word, makes a host call whose stop
  handler mutates `r0` and runs a nested call, and returns through the fixed SVC; all
  stopped CPU state is restored and `call_depth` is 1 inside the handler, 0 after;
- a Thumb target called from ARM state;
- a Thumb target called while the stopped CPSR has ITSTATE (`EQ`, one instruction, Z
  clear) and the E bit set: the call must still return 7 and restore that CPSR exactly;
- a function that moves `sp` before returning: the return svc is passed to the stop
  handler instead of ending the call;
- a stop handler that fails the call, and a call whose stack arguments do not fit.

The complete file is shown in Task 2 Step 5 because its process-level part needs Task 2.

- [x] **Step 2: Add the call contract and implementation**

In `guest_thread.h`:

Committed `core/include/zb/guest_thread.h (constants)`:

```cpp
// svc immediate used to return from a host->guest call.
inline constexpr std::uint32_t kHostReturnSwi = 0x5AFFFF;
inline constexpr std::uint32_t kHostReturnAddress = 0xFFFF0F00;
// Translated-code cache of one JIT. Carrier JITs only run thread start-up and parking.
inline constexpr std::size_t kDefaultCodeCacheSize = 32 * 1024 * 1024;
inline constexpr std::size_t kCarrierCodeCacheSize = 2 * 1024 * 1024;

struct GuestResult {
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
};
```

Committed `core/include/zb/guest_thread.h (handler type)`:

```cpp
using GuestStopHandler = std::function<bool(const Stop&)>;
```

Committed `core/include/zb/guest_thread.h (constructor and call)`:

```cpp
    // precise_faults: a memory fault stops at the faulting instruction with all guest registers
    // committed (needed by guests whose SIGSEGV handlers resume, e.g. Mono). It disables
    // Dynarmic's GetSetElimination, which costs roughly 2x on integer-heavy code.
    GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id,
                bool precise_faults = false, std::size_t code_cache_size = kDefaultCodeCacheSize);
    ~GuestThread() override;

    std::array<std::uint32_t, 16>& regs();
    std::array<std::uint32_t, 64>& ext_regs();
    std::uint32_t cpsr() const;
    void set_cpsr(std::uint32_t value);
    std::uint32_t fpscr() const;
    void set_fpscr(std::uint32_t value);
    std::uint32_t tls() const { return tpidruro_; }
    void set_tls(std::uint32_t value) { tpidruro_ = value; }
    std::size_t processor_id() const { return processor_id_; }

    // Runs until a callback stops the JIT, or until post_signal() interrupts it (Interrupted).
    Stop run();
    // Runs one nested guest function. The stopped CPU state is restored on every return path.
    // The handler runs outside Dynarmic and returns true to resume or false to fail the call.
    std::optional<GuestResult> call(std::uint32_t target, const GuestCall& args,
                                    const GuestStopHandler& handle_stop);
```

Committed `core/include/zb/guest_thread.h (call depth)`:

```cpp
    // Number of host-to-guest calls active on this thread (call() frames).
    int call_depth = 0;
    // Host tid of the host thread running this guest thread.
    std::int32_t tid = 0;
```

`guest_thread.cpp` passes `code_cache_size` to `cfg.code_cache_size` and defines the
CPSR bits it clears (`kCpsrThumb` 0x20, `kCpsrEndian` 0x200, `kCpsrItMask` 0x0600FC00).
The call restores CPU state but not TLS, signal masks, or other emulated kernel state
changed by the guest function:

Committed `core/src/guest_thread.cpp (GuestThread::call)`:

```cpp
std::optional<GuestResult> GuestThread::call(std::uint32_t target, const GuestCall& args,
                                             const GuestStopHandler& handle_stop) {
    const auto saved_regs = regs();
    const auto saved_ext = ext_regs();
    const std::uint32_t saved_cpsr = cpsr();
    const std::uint32_t saved_fpscr = fpscr();

    const auto restore = [&] {
        regs() = saved_regs;
        ext_regs() = saved_ext;
        set_cpsr(saved_cpsr);
        set_fpscr(saved_fpscr);
    };

    const std::uint64_t bytes = static_cast<std::uint64_t>(args.stack.size()) * 4;
    if (bytes > saved_regs[13]) return std::nullopt;
    const std::uint32_t call_sp = static_cast<std::uint32_t>((saved_regs[13] - bytes) & ~7u);
    std::uint8_t* stack = mem_.host_ptr(call_sp, bytes, kPageWrite);
    if (bytes != 0 && stack == nullptr) return std::nullopt;
    if (bytes != 0) std::memcpy(stack, args.stack.data(), static_cast<std::size_t>(bytes));

    regs()[0] = args.regs[0];
    regs()[1] = args.regs[1];
    regs()[2] = args.regs[2];
    regs()[3] = args.regs[3];
    regs()[13] = call_sp;
    regs()[14] = kHostReturnAddress;
    regs()[15] = target & ~1u;
    // A fresh call starts outside any IT block with little-endian data; only T follows the target.
    set_cpsr((saved_cpsr & ~(kCpsrThumb | kCpsrItMask | kCpsrEndian)) | ((target & 1u) ? kCpsrThumb : 0));

    ++call_depth;
    std::optional<GuestResult> result;
    for (;;) {
        const Stop stop = run();
        if (stop.kind == StopKind::Svc && stop.swi == kHostReturnSwi) {
            if (regs()[13] == call_sp) {
                result = GuestResult{regs()[0], regs()[1]};
                break;
            }
            // Not this frame's return: the handler treats the svc as an illegal instruction.
            log("host-to-guest return with sp 0x%08x, frame sp 0x%08x: a longjmp or unwind crossed the "
                "host-to-guest call frame",
                regs()[13], call_sp);
        }
        if (!handle_stop(stop)) break;
    }
    --call_depth;
    restore();
    return result;
}
```

The kuser mapping in `process.cpp` places `0xEF5AFFFF` at offset `0xF00`
(`put(0xf00, kKuserHostReturn, ...)`) before the page becomes RX. The existing helper
offsets are unchanged.

- [x] **Step 3: Verify and commit**

```sh
ninja -C build/host guest_call_test; ctest --test-dir build/host -R '^guest_call_test$' --output-on-failure
```

---

## Task 2: Reusable Process stop dispatch

**Status:** done. Commits `fcaef77` (dispatch) and `a15b86c` (review fixes).

**Files:**
- Modify: `core/include/zb/process.h`, `core/src/process.cpp`, `core/src/syscalls.cpp`
- Create: `guest/tests/host_call_static.c`, `tests/host/host_call_dispatch_test.cpp`
- Modify: `tools/build_guest.sh`, `tests/host/CMakeLists.txt`, `tests/host/check.h`

**Interfaces:** `Process::set_host_call_handler`, `Process::call_guest`,
`Process::dispatch_stop`, `Process::after_stop`, `Process::fault_or_crash`,
`host_call_name`.

- [x] **Step 1: Add the failing dispatch test**

The static guest performs `svc #0x5afe10`, checks the value returned in `r0`, and exits.

Committed `guest/tests/host_call_static.c`:

```c
#include <stdint.h>

int main(void) {
    register uint32_t r0 __asm__("r0") = 7;
    __asm__ volatile("svc #0x5afe10" : "+r"(r0) : : "memory");
    return r0 == 49 ? 0 : 1;
}
```

Committed `tests/host/host_call_dispatch_test.cpp (Task 2 version; Task 5 extends it)`:

```cpp
#include <string>

#include "check.h"
#include "zb/process.h"

int main(int argc, char** argv) {
    CHECK(argc == 2);
    zb::Process process;
    bool called = false;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFE10) return false;
        CHECK(thread.regs()[0] == 7);
        thread.regs()[0] = 49;
        called = true;
        return true;
    });
    CHECK(process.run(argv[1], {argv[1]}, {}) == 0);
    CHECK(called);
    std::puts("host_call_dispatch_test PASS");
    return 0;
}
```

`host_call_dispatch_test` and `guest_call_test` are declared separately in
`tests/host/CMakeLists.txt` with `${CMAKE_SOURCE_DIR}/build/guest/host_call_static` as
their argument.

- [x] **Step 2: Extract one stop dispatcher**

Committed `core/include/zb/process.h (public)`:

```cpp
    // Handles svc #(0x5A0000 | index) before the generated host-call table; returns true if it
    // handled the index. Set before run().
    void set_host_call_handler(HostCallHandler handler) { host_call_handler_ = std::move(handler); }
    // Runs a guest function on `thread`, which is stopped outside Dynarmic (typically inside a
    // host call), through the same stop dispatch as the thread's own loop. Returns nullopt when
    // the call cannot be laid out or the guest cannot continue:
    // - a fatal fault or signal, or exit_group, with other guest threads alive ends the host
    //   process; on the only guest thread it fails the call with exiting() set, and run()
    //   later returns the status;
    // - a thread exit (exit, pthread_exit) inside the call always ends the host process.
    std::optional<GuestResult> call_guest(GuestThread& thread, std::uint32_t target, const GuestCall& args);
```

Committed `core/include/zb/process.h (private)`:

```cpp
    // Handles one stop; true if the thread may resume.
    bool dispatch_stop(GuestThread& thread, const Stop& stop);
    // Delivers pending unblocked signals after a stop; false if one ended the guest.
    bool after_stop(GuestThread& thread);
    // A fault or exception becomes a guest signal, or a crash report and process exit.
    bool fault_or_crash(GuestThread& thread, const Stop& stop);
```

The dispatcher keeps syscall, fault, signal, crash and exit behavior of the old loop, and
adds the D3/D4 paths:

Committed `core/src/process.cpp (dispatch)`:

```cpp
bool Process::dispatch_stop(GuestThread& thread, const Stop& stop) {
    switch (stop.kind) {
    case StopKind::Svc:
        if (stop.swi == 0) {
            if (handle_syscall(*this, thread)) return true;
            if (!exiting_ && thread.call_depth > 0) {
                // bionic has already released this thread's TLS and stack; nothing may run on it.
                log("guest thread exited inside a host-to-guest call");
                request_exit(1);
                exit_host_process();
            }
            if (exiting_ && thread_count() > 1) exit_host_process();
            return false;
        }
        if (stop.swi == kHostReturnSwi) {
            // GuestThread::call consumes its own return; any other one is an illegal instruction.
            if (thread.call_depth == 0) log("host return svc outside a host-to-guest call at pc 0x%08x", stop.pc - 4);
            Stop illegal;
            illegal.kind = StopKind::Exception;
            illegal.exception = Dynarmic::A32::Exception::UndefinedInstruction;
            illegal.pc = stop.pc - 4;
            return fault_or_crash(thread, illegal);
        }
        if ((stop.swi & 0xFF0000u) == kHostCallBase) {
            const std::uint32_t index = stop.swi & 0xFFFFu;
            if (host_call_handler_ && host_call_handler_(index, thread)) return !exiting_;
            if (first_time(kSeenHostCall | index)) {
                const auto [library, name] = host_call_name(index);
                log("host call %s:%s is not implemented yet", library, name);
            }
            thread.regs()[0] = 0;
            return true;
        }
        if (first_time(kSeenUnexpectedSvc | stop.swi)) {
            log("unexpected svc #0x%x at pc 0x%08x", stop.swi, stop.pc);
        }
        thread.regs()[0] = static_cast<std::uint32_t>(-ENOSYS);
        return true;
    case StopKind::Interrupted:
        return true;
    case StopKind::MemoryFault:
    case StopKind::Exception:
        return fault_or_crash(thread, stop);
    case StopKind::None:
        log("guest stopped without a reason at pc 0x%08x", stop.pc);
        request_exit(1);
        if (thread_count() > 1) exit_host_process();
        return false;
    }
    return false;
}

bool Process::fault_or_crash(GuestThread& thread, const Stop& stop) {
    if (deliver_fault(thread, stop)) return true;
    crash_report(stop, thread);
    request_exit(128 + (stop.kind == StopKind::MemoryFault ? SIGSEGV : SIGILL));
    if (thread_count() > 1) exit_host_process();
    return false;
}

bool Process::after_stop(GuestThread& thread) {
    if (!thread.has_pending_signals(thread.sigmask) || dispatch_pending_signals(thread)) return true;
    if (thread_count() > 1) exit_host_process();
    return false;
}

void Process::thread_loop(GuestThread& thread) {
    set_current_thread(&thread);
    while (dispatch_stop(thread, thread.run()) && after_stop(thread)) {
    }
}

std::optional<GuestResult> Process::call_guest(GuestThread& thread, std::uint32_t target, const GuestCall& args) {
    return thread.call(target, args, [&](const Stop& stop) { return dispatch_stop(thread, stop) && after_stop(thread); });
}
```

`host_call_name(index)` looks the index up in `kHostCallNames` and returns `{"?", "?"}`
when it is absent.

- [x] **Step 3: Make guest tid explicit**

`NR_gettid` and the return of `NR_set_tid_address` use `thread.tid` when it is nonzero:

```cpp
const auto guest_tid = [&] {
    return thread.tid != 0 ? thread.tid : static_cast<std::int32_t>(::syscall(SYS_gettid));
};
```

`crash_report` prints the guest tid next to the host tid when they differ:

Committed `core/src/process.cpp (crash_report)`:

```cpp
    const long host_tid = ::syscall(SYS_gettid);
    if (thread.tid != 0 && thread.tid != host_tid) {
        log("  cpsr %08x  tls %08x  tid %ld  guest tid %d", thread.cpsr(), thread.tls(), host_tid, thread.tid);
    } else {
        log("  cpsr %08x  tls %08x  tid %ld", thread.cpsr(), thread.tls(), host_tid);
    }
```

- [x] **Step 4: Verify and commit the dispatch**

```sh
tools/build_guest.sh; ninja -C build/host host_call_dispatch_test; ctest --test-dir build/host -R '^host_call_dispatch_test$' --output-on-failure
```

- [x] **Step 5: Review fixes (commit `a15b86c`)**

A failed `CHECK` may run on a worker thread while guest threads still run, so it must
not call `exit`:

Committed `tests/host/check.h`:

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>

// _Exit, not exit: a failed check may run on a worker thread while guest threads still run,
// and exit() would run static destructors under them or hang.
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stderr);                                                      \
            std::_Exit(1);                                                            \
        }                                                                             \
    } while (0)
```

The complete `guest_call_test.cpp`. Its process-level part runs `host_call_static` in
forked children and, from host call `0xFE10`, maps test code at `0x60000000`:

- a signal posted during a call (host call `0xFE11`) runs the guest handler, returns
  through `rt_sigreturn`, and the call returns `SIGUSR1 + 1` with state intact;
- `exit` inside a call ends the host process with status 1 and the log line
  "guest thread exited inside a host-to-guest call";
- a return svc outside a call exits with `128 + SIGILL`;
- a return with the wrong `sp` fails the call and exits with `128 + SIGILL`;
- a fatal signal inside a call on the only guest thread fails the call and `run()`
  returns `128 + SIGUSR2`.

Committed `tests/host/guest_call_test.cpp`:

```cpp
// Nested host-to-guest calls: CPU state, Thumb and IT entry, nested calls from stop handlers,
// the return-sp check, and (through a real Process running host_call_static) signals, exit
// and stray returns inside calls.
// Usage: guest_call_test <build/guest/host_call_static>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/native_call.h"
#include "zb/process.h"

namespace {

constexpr std::uint32_t kCpsrUser = 0x10;
constexpr std::uint32_t kCpsrThumb = 0x20;
constexpr std::uint32_t kCpsrEndian = 0x200;
// ITSTATE = 0x08: one instruction under condition EQ, which fails with Z clear.
constexpr std::uint32_t kCpsrItEqOne = 0x800;

void put32(zb::GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    std::memcpy(mem.base() + addr, &value, sizeof value);
}

void put16(zb::GuestMemory& mem, std::uint32_t addr, std::uint16_t value) {
    std::memcpy(mem.base() + addr, &value, sizeof value);
}

std::uint32_t get32(zb::GuestMemory& mem, std::uint32_t addr) {
    std::uint32_t value;
    std::memcpy(&value, mem.base() + addr, sizeof value);
    return value;
}

void thread_level_tests() {
    zb::GuestMemory mem;
    CHECK(mem.ok());
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x20000, 0x2000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0xFFFF0000, 0x1000, PROT_READ | PROT_WRITE));

    put32(mem, 0x10000, 0xE0800001);  // add r0, r0, r1
    put32(mem, 0x10004, 0xE59D2000);  // ldr r2, [sp]
    put32(mem, 0x10008, 0xE0800002);  // add r0, r0, r2
    put32(mem, 0x1000C, 0xEF5A0010);  // svc #0x5a0010
    put32(mem, 0x10010, 0xE12FFF1E);  // bx lr
    put32(mem, 0x10020, 0xE0800001);  // add r0, r0, r1
    put32(mem, 0x10024, 0xE12FFF1E);  // bx lr
    put16(mem, 0x10040, 0x9A00);      // thumb: ldr r2, [sp]
    put16(mem, 0x10042, 0x1880);      // thumb: adds r0, r0, r2
    put16(mem, 0x10044, 0x4770);      // thumb: bx lr
    put32(mem, 0x10060, 0xE28DD008);  // add sp, sp, #8
    put32(mem, 0x10064, 0xE12FFF1E);  // bx lr
    put32(mem, zb::kHostReturnAddress, 0xEF5AFFFF);  // svc #0x5affff
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    CHECK(mem.protect(0xFFFF0000, 0x1000, PROT_READ | PROT_EXEC));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(mem, &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs().fill(0xA5A5A5A5);
    thread.ext_regs().fill(0x5A5A5A5A);
    thread.regs()[13] = 0x22000;
    thread.regs()[15] = 0x12345678;
    thread.set_cpsr(kCpsrUser);
    thread.set_fpscr(0x01000000);
    const auto saved_regs = thread.regs();
    const auto saved_ext = thread.ext_regs();
    CHECK(thread.call_depth == 0);

    // A stop handler mutates the call registers and makes a nested call of its own.
    zb::GuestCall call;
    call.regs = {10, 20, 0, 0};
    call.stack = {12};
    int host_calls = 0;
    const auto result = thread.call(0x10000, call, [&](const zb::Stop& stop) {
        CHECK(stop.kind == zb::StopKind::Svc && stop.swi == 0x5A0010);
        CHECK(thread.call_depth == 1);
        CHECK(thread.regs()[0] == 42 && thread.regs()[1] == 20);
        ++host_calls;
        zb::GuestCall inner;
        inner.regs = {5, 6, 0, 0};
        const auto nested = thread.call(0x10020, inner, [](const zb::Stop&) { return false; });
        CHECK(nested && nested->r0 == 11);
        CHECK(thread.call_depth == 1);
        CHECK(thread.regs()[0] == 42 && thread.regs()[1] == 20);
        thread.regs()[0] += 100 + nested->r0;
        return true;
    });
    CHECK(result && result->r0 == 153 && result->r1 == 20);
    CHECK(host_calls == 1);
    CHECK(thread.call_depth == 0);
    CHECK(thread.regs() == saved_regs);
    CHECK(thread.ext_regs() == saved_ext);
    CHECK(thread.cpsr() == kCpsrUser && thread.fpscr() == 0x01000000);

    // Thumb target from an ARM caller.
    zb::GuestCall thumb;
    thumb.regs = {3, 0, 0, 0};
    thumb.stack = {4};
    const auto thumb_result = thread.call(0x10041, thumb, [](const zb::Stop&) { return false; });
    CHECK(thumb_result && thumb_result->r0 == 7);
    CHECK(thread.cpsr() == kCpsrUser);

    // Stopped inside a Thumb IT block with the E bit set: the call must start with a clean
    // ITSTATE and little-endian data, then restore the stopped CPSR exactly.
    const std::uint32_t it_cpsr = kCpsrUser | kCpsrThumb | kCpsrItEqOne | kCpsrEndian;
    thread.set_cpsr(it_cpsr);
    const auto it_result = thread.call(0x10041, thumb, [](const zb::Stop&) { return false; });
    CHECK(it_result && it_result->r0 == 7);
    CHECK(thread.cpsr() == it_cpsr);
    thread.set_cpsr(kCpsrUser);

    // A return whose sp differs from the call frame is not a return: the handler sees it.
    bool saw_bad_return = false;
    const auto bad_sp = thread.call(0x10060, zb::GuestCall{}, [&](const zb::Stop& stop) {
        saw_bad_return = stop.kind == zb::StopKind::Svc && stop.swi == zb::kHostReturnSwi;
        return false;
    });
    CHECK(!bad_sp && saw_bad_return);
    CHECK(thread.call_depth == 0);
    CHECK(thread.regs() == saved_regs);

    // A stop handler that fails the call.
    const auto failed = thread.call(0x10000, call, [](const zb::Stop&) { return false; });
    CHECK(!failed && thread.call_depth == 0 && thread.regs() == saved_regs);

    zb::GuestCall too_large;
    too_large.stack.resize(0x1000);
    thread.regs()[13] = 0x20004;
    CHECK(!thread.call(0x10000, too_large, [](const zb::Stop&) { return false; }));
    CHECK(thread.call_depth == 0);
}

// Guest code mapped into a running Process from inside its first host call.
constexpr std::uint32_t kCode = 0x60000000;
constexpr std::uint32_t kData = 0x61000000;
constexpr std::uint32_t kSignalCall = kCode + 0x00;  // svc #0x5afe11 (posts r0); add r0, #1
constexpr std::uint32_t kHandler = kCode + 0x0C;     // *kData = r0
constexpr std::uint32_t kRestorer = kCode + 0x18;    // rt_sigreturn
constexpr std::uint32_t kExitCall = kCode + 0x20;    // exit(r0)
constexpr std::uint32_t kBadSpCall = kCode + 0x28;   // add sp, #8; bx lr

void map_call_code(zb::GuestMemory& mem) {
    CHECK(mem.range_free(kCode, 0x1000) && mem.range_free(kData, 0x1000));
    CHECK(mem.map_anon(kCode, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    const std::uint32_t words[] = {
        0xEF5AFE11, 0xE2800001, 0xE12FFF1E,  // signal call
        0xE3A03461, 0xE5830000, 0xE12FFF1E,  // handler: mov r3, #0x61000000; str r0, [r3]
        0xE3A070AD, 0xEF000000,              // restorer: mov r7, #173; svc #0
        0xE3A07001, 0xEF000000,              // exit: mov r7, #1; svc #0
        0xE28DD008, 0xE12FFF1E,              // bad sp
    };
    for (std::size_t i = 0; i < sizeof words / sizeof words[0]; ++i) put32(mem, kCode + 4 * i, words[i]);
    CHECK(mem.protect(kCode, 0x1000, PROT_READ | PROT_EXEC));
}

struct ChildResult {
    int status = -1;  // exit code, or -1 if the child did not exit normally
    std::string err;
};

// Runs `body` in a forked child with stderr captured; the child exits with body's value.
ChildResult run_in_child(const std::function<int()>& body) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        close(fds[1]);
        const int status = body();
        std::fflush(stderr);
        std::_Exit(status & 0xff);
    }
    close(fds[1]);
    ChildResult result;
    char buffer[1024];
    for (;;) {
        const ssize_t n = read(fds[0], buffer, sizeof buffer);
        if (n <= 0) break;
        result.err.append(buffer, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    int wstatus = 0;
    CHECK(waitpid(pid, &wstatus, 0) == pid);
    if (WIFEXITED(wstatus)) result.status = WEXITSTATUS(wstatus);
    return result;
}

// Runs host_call_static; its host call 0xFE10 runs `body` on the main guest thread, and host
// call 0xFE11 posts the signal in r0 to the calling thread. Returns the Process::run status.
int run_scenario(const char* exe, const std::function<void(zb::Process&, zb::GuestThread&)>& body) {
    zb::Process process;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index == 0xFE10) {
            map_call_code(process.memory());
            thread.regs()[0] = 49;
            body(process, thread);
            return true;
        }
        if (index == 0xFE11) {
            zb::g::siginfo32 info{};
            info.si_signo = static_cast<std::int32_t>(thread.regs()[0]);
            info.si_code = SI_TKILL;
            thread.post_signal(info);
            return true;
        }
        return false;
    });
    return process.run(exe, {exe}, {});
}

void process_level_tests(const char* exe) {
    // A signal posted during a call runs the guest handler, returns through rt_sigreturn, and
    // the call then completes with the stopped state intact.
    ChildResult signal = run_in_child([&] {
        return run_scenario(exe, [](zb::Process& process, zb::GuestThread& thread) {
            process.sigactions[SIGUSR1] = {kHandler, 0x04000004 /* SA_SIGINFO | SA_RESTORER */, kRestorer, {0, 0}};
            const auto saved = thread.regs();
            zb::GuestCall args;
            args.regs = {static_cast<std::uint32_t>(SIGUSR1), 0, 0, 0};
            const auto result = process.call_guest(thread, kSignalCall, args);
            CHECK(result && result->r0 == static_cast<std::uint32_t>(SIGUSR1) + 1);
            CHECK(get32(process.memory(), kData) == static_cast<std::uint32_t>(SIGUSR1));
            CHECK(thread.regs() == saved && thread.sigmask == 0 && thread.call_depth == 0);
        });
    });
    std::fputs(signal.err.c_str(), stderr);
    CHECK(signal.status == 0);

    // A thread exit inside a call ends the host process: bionic has already torn that thread down.
    ChildResult exited = run_in_child([&] {
        run_scenario(exe, [](zb::Process& process, zb::GuestThread& thread) {
            zb::GuestCall args;
            args.regs = {5, 0, 0, 0};
            (void)process.call_guest(thread, kExitCall, args);
            std::_Exit(3);  // must not be reached
        });
        return 4;
    });
    CHECK(exited.status == 1);
    CHECK(exited.err.find("guest thread exited inside a host-to-guest call") != std::string::npos);

    // The return svc outside any call is an illegal instruction.
    ChildResult stray = run_in_child([&] {
        return run_scenario(exe, [](zb::Process&, zb::GuestThread& thread) {
            thread.regs()[15] = zb::kHostReturnAddress;
        });
    });
    CHECK(stray.status == 128 + SIGILL);
    CHECK(stray.err.find("host return svc outside a host-to-guest call") != std::string::npos);

    // A return with the wrong sp (longjmp or unwind across the frame) is fatal the same way.
    ChildResult bad_sp = run_in_child([&] {
        return run_scenario(exe, [](zb::Process& process, zb::GuestThread& thread) {
            CHECK(!process.call_guest(thread, kBadSpCall, zb::GuestCall{}));
            CHECK(process.exiting() && thread.call_depth == 0);
        });
    });
    CHECK(bad_sp.status == 128 + SIGILL);
    CHECK(bad_sp.err.find("crossed the host-to-guest call frame") != std::string::npos);

    // A fatal signal inside a call on the only guest thread fails the call and ends run() with
    // the signal status instead of ending the host process.
    ChildResult fatal = run_in_child([&] {
        return run_scenario(exe, [](zb::Process& process, zb::GuestThread& thread) {
            zb::GuestCall args;
            args.regs = {static_cast<std::uint32_t>(SIGUSR2), 0, 0, 0};
            CHECK(!process.call_guest(thread, kSignalCall, args));
            CHECK(process.exiting() && process.exit_status() == 128 + SIGUSR2);
        });
    });
    CHECK(fatal.status == 128 + SIGUSR2);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    thread_level_tests();
    process_level_tests(argv[1]);
    std::puts("guest_call_test PASS");
    return 0;
}
```

```sh
tools/build_guest.sh; ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/run_guest_tests.sh
```

Expected: 13/13 host tests, all guest cases pass (`or_dlopen_dynamic` skips without the
APK).

---

## Task 3: Fixed `zbhost` service protocol

**Status:** done. Commit `49608ec`. Task 4 replaces this version 1 protocol with version 2.

**Files:**
- Create: `core/include/zb/library_protocol.h`, `guest/zbhost/zbhost.c`,
  `tests/host/zbhost_protocol_test.cpp`
- Modify: `tools/build_guest.sh`, `tests/host/CMakeLists.txt`

**Interfaces:** `zb_service_api`, `ZB_SERVICE_READY_INDEX`, `ZB_CARRIER_PARK_INDEX`,
`ZB_SERVICE_PROTOCOL_VERSION`.

- [x] **Step 1: Write the failing handshake test and define the protocol**

Committed `tests/host/zbhost_protocol_test.cpp (version 1)`:

```cpp
#include <cstring>
#include <string>

#include "check.h"
#include "zb/library_protocol.h"
#include "zb/process.h"

int main(int argc, char** argv) {
    CHECK(argc == 4);
    zb::Process process;
    process.set_sysroot(argv[1]);
    bool ready = false;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != ZB_SERVICE_READY_INDEX) return false;
        const std::uint8_t* source = process.memory().host_ptr(
            thread.regs()[0], sizeof(zb_service_api), zb::kPageRead);
        CHECK(source != nullptr);
        zb_service_api api;
        std::memcpy(&api, source, sizeof api);
        CHECK(api.size == sizeof api && api.version == ZB_SERVICE_PROTOCOL_VERSION);
        CHECK(api.dlopen_fn != 0 && api.dlsym_fn != 0 && api.dlerror_fn != 0);
        CHECK(api.spawn_carrier_fn != 0 && api.scratch_size == ZB_SERVICE_SCRATCH_SIZE);
        CHECK(process.memory().host_ptr(api.scratch, api.scratch_size, zb::kPageWrite) != nullptr);
        ready = true;
        thread.regs()[0] = 0;
        return true;
    });
    const std::string library_path = std::string("LD_LIBRARY_PATH=") + argv[3];
    CHECK(process.run(argv[2], {argv[2], "16"}, {library_path}) == 0);
    CHECK(ready);
    std::puts("zbhost_protocol_test PASS");
    return 0;
}
```

Committed `core/include/zb/library_protocol.h (version 1)`:

```c
#pragma once

#include <stdint.h>

#define ZB_SERVICE_PROTOCOL_VERSION 1u
#define ZB_SERVICE_READY_INDEX 0xFE00u
#define ZB_CARRIER_PARK_INDEX 0xFE01u
#define ZB_SERVICE_SCRATCH_SIZE 4096u

struct zb_service_api {
    uint32_t size;
    uint32_t version;
    uint32_t dlopen_fn;
    uint32_t dlsym_fn;
    uint32_t dlerror_fn;
    uint32_t spawn_carrier_fn;
    uint32_t scratch;
    uint32_t scratch_size;
};
```

Committed `guest/zbhost/zbhost.c (version 1)`:

```c
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "zb/library_protocol.h"

static char scratch[ZB_SERVICE_SCRATCH_SIZE];

static uint32_t host_call(uint32_t index, uint32_t arg) {
    register uint32_t r0 __asm__("r0") = arg;
    if (index == ZB_SERVICE_READY_INDEX) {
        __asm__ volatile("svc #0x5afe00" : "+r"(r0) : : "memory");
    } else {
        __asm__ volatile("svc #0x5afe01" : "+r"(r0) : : "memory");
    }
    return r0;
}

static uint32_t service_dlopen(const char* path, uint32_t flags) {
    return (uint32_t)(uintptr_t)dlopen(path, (int)flags);
}

static uint32_t service_dlsym(uint32_t handle, const char* name) {
    return (uint32_t)(uintptr_t)dlsym((void*)(uintptr_t)handle, name);
}

static uint32_t service_dlerror(void) {
    return (uint32_t)(uintptr_t)dlerror();
}

static void* carrier_main(void* unused) {
    (void)unused;
    host_call(ZB_CARRIER_PARK_INDEX, 0);
    return NULL;
}

static uint32_t spawn_carrier(void) {
    pthread_t thread;
    const int rc = pthread_create(&thread, NULL, carrier_main, NULL);
    if (rc == 0) pthread_detach(thread);
    return (uint32_t)rc;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* dl_android = dlopen("libdl_android.so", RTLD_NOW);
    void (*set_target_sdk)(unsigned) = dl_android != NULL
        ? (void (*)(unsigned))dlsym(dl_android, "android_set_application_target_sdk_version")
        : NULL;
    if (set_target_sdk == NULL) return 3;
    set_target_sdk((unsigned)strtoul(argv[1], NULL, 10));
    (void)dlopen("libzbcompat.so", RTLD_NOW | RTLD_GLOBAL);
    const struct zb_service_api api = {
        sizeof(api), ZB_SERVICE_PROTOCOL_VERSION,
        (uint32_t)(uintptr_t)service_dlopen,
        (uint32_t)(uintptr_t)service_dlsym,
        (uint32_t)(uintptr_t)service_dlerror,
        (uint32_t)(uintptr_t)spawn_carrier,
        (uint32_t)(uintptr_t)scratch, sizeof(scratch),
    };
    return (int)host_call(ZB_SERVICE_READY_INDEX, (uint32_t)(uintptr_t)&api);
}
```

`tools/build_guest.sh` builds it after the dynamic-C loop:

```sh
"$CC" -O2 -Wall -I"$ROOT/core/include" -o "$OUT/zbhost" \
    "$ROOT/guest/zbhost/zbhost.c" -ldl
```

- [x] **Step 2: Verify and commit**

```sh
tools/build_guest.sh; ninja -C build/host zbhost_protocol_test; ctest --test-dir build/host -R '^zbhost_protocol_test$' --output-on-failure
```

---

## Task 4: Service-thread library runtime

**Status:** ready, plan revised after review (D1, D5, D6, D8, D10, D11).

**Files:**
- Create: `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp`
- Create: `guest/testlib/zbcallprobe.c`, `tests/host/library_runtime_test.cpp`
- Modify: `core/include/zb/library_protocol.h`, `guest/zbhost/zbhost.c`,
  `tests/host/zbhost_protocol_test.cpp`
- Modify: `core/include/zb/guest_thread.h`, `core/src/guest_thread.cpp`
- Modify: `core/include/zb/process.h`, `core/src/signals.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`, `tools/build_guest.sh`,
  `tools/gen_stubs.py`

**Interfaces:** `LibraryRuntimeOptions`, `LibraryRuntime::{set_host_call_handler, start,
load_library, find_symbol, call_on_service, call_on_current, memory, service_api,
guest_thread_count}`, `GuestThread::{park_token, park, wake}`,
`Process::current_thread`, protocol version 2 (`ZB_SERVICE_AGAIN`, `ZB_GUEST_RTLD_*`,
`ZB_HOST_EXIT_PRELOAD`, `malloc_fn`, `free_fn`, `ZB_RUNTIME_HOST_CALL_FIRST/LAST`).

Design notes:

- **Service loop.** `zbhost` calls `READY` with its API. The first `READY` validates the API
  and publishes readiness; every `READY` then serves queued commands on that thread through
  `Process::call_guest`. Commands never run under the queue mutex.
- **Parking (D6).** When the queue is empty the service thread reads its park token,
  checks pending unblocked signals and the queue, and sleeps in `FUTEX_WAIT` on the token.
  `post_signal` and `submit` change the token and `FUTEX_WAKE` it. With a pending
  unblocked signal, `READY` returns `ZB_SERVICE_AGAIN`; zbhost loops, and `thread_loop`
  delivers the signal between the two host calls.
- **Lifetime (D5).** There is no shutdown. After a successful start the destructor logs
  "LibraryRuntime destroyed after start; the runtime is process-lifetime" and aborts.
  Tests finish with `std::fflush(stdout); std::_Exit(0);`. A failed start whose zbhost
  already exited can be destroyed normally.
- **Preload (D11).** `zbhost <target_sdk> [<preload>]` preloads `libzbcompat.so` and the
  optional library with `RTLD_GLOBAL`; a failure prints the guest `dlerror` and exits
  with `ZB_HOST_EXIT_PRELOAD` (4), which `start` reports as
  "zbhost could not preload a library (status 4)". `start` waits at most
  `ready_timeout` (10 s) for `READY`.
- **Hooks (D8).** The runtime owns host-call indices `0xFE00-0xFEFF` and chains every
  other index to the handler set before `start`. `call_on_current` makes a nested call on
  the guest thread the calling host thread runs, for host-call handlers.
- **Misuse (D10).** A service request made on the service thread fails with an error
  naming `call_on_current` instead of deadlocking.

- [ ] **Step 1: Add the probe fixture and the failing tests**

The probe library exports base-AAPCS functions for every JNI return type, a mixed softfp
signature, thread identity, a chained host call (`svc #0x5afd00`), a concurrency
rendezvous, bionic contention, and signal helpers. Task 5 uses the carrier helpers.

Write `guest/testlib/zbcallprobe.c` with exactly this content:

```c
/* Guest probe library for library_runtime_test: return types, softfp arguments, thread
 * identity, bionic contention, signals and runtime host calls. Every export uses base AAPCS. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default"), pcs("aapcs")))
#define DATA __attribute__((visibility("default")))

API int32_t zb_probe_mix(uint32_t env, uint32_t self, uint8_t z, int8_t b, uint16_t c, int16_t s, int32_t i,
                         int64_t j, float f, double d, uint32_t ref) {
    if (env != 0xe000 || self != 0x1077 || z != 1 || b != -2 || c != 0x1234 || s != -3 || i != 4 ||
        j != INT64_C(0x1122334455667788) || f != 1.5f || d != -2.25 || ref != 0x1099) {
        return -1;
    }
    return 42;
}

API void zb_return_v(void) {}
API uint8_t zb_return_z(void) { return 1; }
API int8_t zb_return_b(void) { return -2; }
API uint16_t zb_return_c(void) { return 0x1234; }
API int16_t zb_return_s(void) { return -3; }
API int32_t zb_return_i(void) { return 42; }
API int64_t zb_return_j(void) { return INT64_C(0x1122334455667788); }
API float zb_return_f(void) { return 3.5f; }
API double zb_return_d(void) { return -1.25; }
API uint32_t zb_return_l(void) { return 0x12345; }

API uint32_t zb_probe_tls(void) {
    return (uint32_t)(uintptr_t)__builtin_thread_pointer();
}

API int32_t zb_probe_tid(void) {
    return (int32_t)syscall(__NR_gettid);
}

API int32_t zb_probe_cached_tid(void) {
    return (int32_t)gettid();
}

/* Runtime host call outside the generated stub range, answered by a chained handler. */
API uint32_t zb_probe_host_call(uint32_t value) {
    register uint32_t r0 __asm__("r0") = value;
    __asm__ volatile("svc #0x5afd00" : "+r"(r0) : : "memory");
    return r0;
}

static uint32_t overlap_count;

/* Returns only after two callers are inside at the same time (or -1 after 2 s). */
API int32_t zb_probe_overlap(void) {
    const uint32_t ticket = __atomic_add_fetch(&overlap_count, 1, __ATOMIC_SEQ_CST);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (__atomic_load_n(&overlap_count, __ATOMIC_SEQ_CST) < 2) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec;
        if (elapsed > 2000000000LL) return -1;
        sched_yield();
    }
    return (int32_t)ticket;
}

static pthread_mutex_t contention_lock = PTHREAD_MUTEX_INITIALIZER;
DATA uint32_t zb_contention_total;

/* Mutex, malloc/free, errno and thread identity under concurrency. 0 on success. */
API int32_t zb_probe_contention(uint32_t iterations) {
    const pthread_t self = pthread_self();
    const pid_t tid = gettid();
    for (uint32_t i = 0; i < iterations; ++i) {
        if (pthread_mutex_lock(&contention_lock) != 0) return -1;
        ++zb_contention_total;
        if (pthread_mutex_unlock(&contention_lock) != 0) return -2;
        char* block = malloc(16 + (i % 512));
        if (block == NULL) return -3;
        memset(block, (int)(i & 0xff), 16);
        free(block);
        const int expected = (int)((tid ^ i) & 0x7fff) + 1;
        errno = expected;
        sched_yield();
        if (errno != expected) return -4;
        if (!pthread_equal(pthread_self(), self) || gettid() != tid || syscall(__NR_gettid) != tid) return -5;
    }
    return 0;
}

DATA volatile int32_t zb_usr1_count;
DATA volatile int32_t zb_usr1_tid;
DATA volatile uint32_t zb_usr1_tls;

static void on_usr1(int sig) {
    (void)sig;
    zb_usr1_tid = (int32_t)syscall(__NR_gettid);
    zb_usr1_tls = (uint32_t)(uintptr_t)__builtin_thread_pointer();
    ++zb_usr1_count;
}

API int32_t zb_probe_install_usr1(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    return sigaction(SIGUSR1, &sa, NULL);
}

/* tgkill to this thread's own (cached) tid. */
API int32_t zb_probe_tgkill_self(int32_t sig) {
    return (int32_t)syscall(__NR_tgkill, getpid(), gettid(), sig);
}

DATA volatile int32_t zb_alarm_count;
DATA volatile int32_t zb_alarm_tid;

static void on_alarm(int sig) {
    (void)sig;
    zb_alarm_tid = (int32_t)syscall(__NR_gettid);
    ++zb_alarm_count;
}

/* One-shot ITIMER_REAL; the handler records the tid it ran on. */
API int32_t zb_probe_arm_alarm(int32_t usec) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0) return -1;
    zb_alarm_count = 0;
    zb_alarm_tid = 0;
    struct itimerval timer;
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_usec = usec;
    return setitimer(ITIMER_REAL, &timer, NULL);
}

API int32_t zb_probe_block_signal(int32_t sig, int32_t block) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    return sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

API int32_t zb_probe_signal_blocked(int32_t sig) {
    sigset_t current;
    if (sigprocmask(SIG_BLOCK, NULL, &current) != 0) return -1;
    return sigismember(&current, sig);
}

/* ss_sp of the enabled alternate signal stack, or 0. */
API uint32_t zb_probe_altstack_sp(void) {
    stack_t ss;
    if (sigaltstack(NULL, &ss) != 0 || (ss.ss_flags & SS_DISABLE) != 0) return 0;
    return (uint32_t)(uintptr_t)ss.ss_sp;
}
```

In `tools/build_guest.sh`, replace:

```sh
#   guest/testlib/zbthrow.cpp   -> build/guest/lib/libzbthrow.so
```

with:

```sh
#   guest/testlib/zbthrow.cpp   -> build/guest/lib/libzbthrow.so
#   guest/testlib/zbcallprobe.c -> build/guest/lib/libzbcallprobe.so
#   guest/zbhost/zbhost.c       -> build/guest/zbhost
```

In `tools/build_guest.sh`, replace:

```sh
for src in "$ROOT"/guest/tests/*_dynamic.cpp; do
```

with:

```sh
# Base AAPCS probe library for library_runtime_test.
"$CC" -shared -fPIC -O2 -Wall -Wl,-soname,libzbcallprobe.so -o "$OUT/lib/libzbcallprobe.so" \
    "$ROOT/guest/testlib/zbcallprobe.c"
for src in "$ROOT"/guest/tests/*_dynamic.cpp; do
```

`-fPIC` is required: the probe exports data symbols that the test reads through
`find_symbol` and `LibraryRuntime::memory`.

In `tests/host/CMakeLists.txt`, replace:

```cmake
# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(async_signal_test async_signal_test.cpp)
```

with:

```cmake
# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(library_runtime_test library_runtime_test.cpp)
target_link_libraries(library_runtime_test PRIVATE zbcore)
add_test(NAME library_runtime_test COMMAND library_runtime_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib/libzbcallprobe.so)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(async_signal_test async_signal_test.cpp)
```

The service-thread test covers guest RTLD constants, `dlopen`/`dlsym`/`dlerror`
(including `NOLOAD` and `RTLD_DEFAULT`), every return type through
`store_native_result`, the mixed softfp signature, host-call chaining with a nested
`call_on_current`, the service-thread misuse guard, preload failure in a child process,
and a SIGALRM delivered to the parked service thread. The alarm check polls guest memory
without running guest code, so it can only pass if the parked thread woke up.

Write `tests/host/library_runtime_test.cpp` with exactly this content:

```cpp
// Library-mode runtime on the service thread: guest RTLD flags, dlopen/dlsym/dlerror, calls of
// every return type, host-call chaining, misuse guards, preload failure, and signal delivery
// to the parked service thread.
// Usage: library_runtime_test <sysroot> <zbhost> <libzbcallprobe.so>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "check.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace {

using namespace std::chrono_literals;

std::uint32_t fbits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t dbits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint32_t read32(zb::LibraryRuntime& runtime, std::uint32_t address) {
    const std::uint8_t* source = runtime.memory().host_ptr(address, 4, zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

// Polls a guest word without running guest code on this host thread.
bool wait_nonzero(zb::LibraryRuntime& runtime, std::uint32_t address, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (read32(runtime, address) == 0) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

zb::LibraryRuntimeOptions runtime_options(char** argv) {
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.envp = {"LD_LIBRARY_PATH=" + std::filesystem::path(argv[3]).parent_path().string()};
    return options;
}

// start() reports a failed preload; runs in a child because a runtime is process-lifetime.
void check_preload_failure(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        zb::LibraryRuntime runtime;
        zb::LibraryRuntimeOptions options = runtime_options(argv);
        options.preload = "libzb-does-not-exist.so";
        std::string error;
        if (runtime.start(options, error)) std::_Exit(10);
        std::fprintf(stderr, "expected start error: %s\n", error.c_str());
        std::_Exit(error.find("status 4") != std::string::npos ? 0 : 11);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string probe = argv[3];
    const std::string libdir = std::filesystem::path(probe).parent_path();
    check_preload_failure(argv);

    CHECK(ZB_GUEST_RTLD_NOW == 0u && ZB_GUEST_RTLD_LAZY == 1u && ZB_GUEST_RTLD_GLOBAL == 2u);
    CHECK(ZB_GUEST_RTLD_NOLOAD == 4u && ZB_GUEST_RTLD_NODELETE == 0x1000u && ZB_GUEST_RTLD_DEFAULT == 0xFFFFFFFFu);

    zb::LibraryRuntime runtime;
    std::uint32_t chained_function = 0;
    int chained_calls = 0;
    runtime.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFD00) return false;
        ++chained_calls;
        std::string misuse;
        CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW, misuse) == 0);
        CHECK(misuse.find("service thread") != std::string::npos);
        const auto nested = runtime.call_on_current(chained_function, zb::GuestCall{});
        CHECK(nested && nested->r0 == 42);
        thread.regs()[0] += nested->r0;
        return true;
    });

    std::string error;
    CHECK(runtime.start(runtime_options(argv), error));
    CHECK(runtime.guest_thread_count() == 1);
    CHECK(runtime.service_api().version == ZB_SERVICE_PROTOCOL_VERSION);
    CHECK(!runtime.call_on_current(runtime.service_api().malloc_fn, zb::GuestCall{}));

    CHECK(runtime.load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == 0);
    error.clear();
    const std::uint32_t library = runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library);
    CHECK(runtime.find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    error.clear();
    CHECK(runtime.find_symbol(ZB_GUEST_RTLD_DEFAULT, "zb_return_i", error) == symbol("zb_return_i"));

    // Every JNI return type, converted with store_native_result.
    struct ReturnCase {
        const char* name;
        char type;
        std::uint64_t x0;
        std::uint64_t d0;
    };
    const ReturnCase returns[] = {
        {"zb_return_z", 'Z', 1, 0},
        {"zb_return_b", 'B', static_cast<std::uint64_t>(-2), 0},
        {"zb_return_c", 'C', 0x1234, 0},
        {"zb_return_s", 'S', static_cast<std::uint64_t>(-3), 0},
        {"zb_return_i", 'I', 42, 0},
        {"zb_return_j", 'J', 0x1122334455667788ull, 0},
        {"zb_return_f", 'F', 0, fbits(3.5f)},
        {"zb_return_d", 'D', 0, dbits(-1.25)},
        {"zb_return_l", 'L', 0x7000012345ull, 0},
    };
    CHECK(runtime.call_on_service(symbol("zb_return_v"), zb::GuestCall{}));
    for (const ReturnCase& expected : returns) {
        const auto result = runtime.call_on_service(symbol(expected.name), zb::GuestCall{});
        CHECK(result);
        zb::NativeRegs regs{};
        zb::store_native_result(expected.type, result->r0, result->r1, regs,
                                [](std::uint32_t handle) { return 0x7000000000ull | handle; });
        if (expected.type == 'F' || expected.type == 'D') {
            CHECK(regs.d[0] == expected.d0);
        } else {
            CHECK(regs.x[0] == expected.x0);
        }
    }

    // softfp argument layout of a mixed JNI signature.
    std::uint64_t host_stack[1] = {0x99};
    zb::NativeRegs host{};
    host.x[1] = 0x77;
    host.x[2] = 1;
    host.x[3] = static_cast<std::uint64_t>(-2);
    host.x[4] = 0x1234;
    host.x[5] = static_cast<std::uint64_t>(-3);
    host.x[6] = 4;
    host.x[7] = 0x1122334455667788ull;
    host.d[0] = fbits(1.5f);
    host.d[1] = dbits(-2.25);
    host.stack = host_stack;
    const auto to_handle = [](std::uint64_t reference) {
        return reference == 0 ? 0u : static_cast<std::uint32_t>(reference + 0x1000);
    };
    const zb::GuestCall mixed = zb::marshal_native_args("IZBCSIJFDL", host, 0xE000, to_handle);
    const auto mixed_result = runtime.call_on_service(symbol("zb_probe_mix"), mixed);
    CHECK(mixed_result && mixed_result->r0 == 42);

    // Host calls outside the runtime range reach the chained handler, which may nest a call.
    chained_function = symbol("zb_return_i");
    zb::GuestCall value;
    value.regs = {100, 0, 0, 0};
    const auto chained = runtime.call_on_service(symbol("zb_probe_host_call"), value);
    CHECK(chained && chained->r0 == 142 && chained_calls == 1);

    // A process-directed SIGALRM reaches the service thread while it is parked in READY.
    const std::uint32_t alarm_count = symbol("zb_alarm_count");
    const std::uint32_t alarm_tid = symbol("zb_alarm_tid");
    const auto service_tid = runtime.call_on_service(symbol("zb_probe_tid"), zb::GuestCall{});
    CHECK(service_tid && service_tid->r0 != 0);
    zb::GuestCall alarm;
    alarm.regs = {20000, 0, 0, 0};
    const auto armed = runtime.call_on_service(symbol("zb_probe_arm_alarm"), alarm);
    CHECK(armed && armed->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);
    const auto after_alarm = runtime.call_on_service(symbol("zb_return_i"), zb::GuestCall{});
    CHECK(after_alarm && after_alarm->r0 == 42);

    CHECK(runtime.guest_thread_count() == 1);
    std::puts("library_runtime_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
```

Write `tests/host/zbhost_protocol_test.cpp` with exactly this content:

```cpp
// zbhost publishes a valid service API through READY, and a failed preload ends it with
// ZB_HOST_EXIT_PRELOAD before READY.
// Usage: zbhost_protocol_test <sysroot> <zbhost> <build/guest/lib>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "check.h"
#include "zb/library_protocol.h"
#include "zb/process.h"

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string library_path = std::string("LD_LIBRARY_PATH=") + argv[3];

    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        zb::Process process;
        process.set_sysroot(argv[1]);
        const int status = process.run(argv[2], {argv[2], "16", "libzb-does-not-exist.so"}, {library_path});
        std::_Exit(status == ZB_HOST_EXIT_PRELOAD ? 0 : 1);
    }
    int wstatus = 0;
    CHECK(waitpid(child, &wstatus, 0) == child);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    zb::Process process;
    process.set_sysroot(argv[1]);
    bool ready = false;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != ZB_SERVICE_READY_INDEX) return false;
        const std::uint8_t* source = process.memory().host_ptr(thread.regs()[0], sizeof(zb_service_api), zb::kPageRead);
        CHECK(source != nullptr);
        zb_service_api api;
        std::memcpy(&api, source, sizeof api);
        CHECK(api.size == sizeof api && api.version == ZB_SERVICE_PROTOCOL_VERSION);
        CHECK(api.dlopen_fn != 0 && api.dlsym_fn != 0 && api.dlerror_fn != 0);
        CHECK(api.spawn_carrier_fn != 0 && api.malloc_fn != 0 && api.free_fn != 0);
        CHECK(api.scratch_size == ZB_SERVICE_SCRATCH_SIZE);
        CHECK(process.memory().host_ptr(api.scratch, api.scratch_size, zb::kPageWrite) != nullptr);
        ready = true;
        thread.regs()[0] = 0;
        return true;
    });
    CHECK(process.run(argv[2], {argv[2], "16"}, {library_path}) == 0);
    CHECK(ready);
    std::puts("zbhost_protocol_test PASS");
    return 0;
}
```

```sh
tools/build_guest.sh; ninja -C build/host library_runtime_test zbhost_protocol_test
```

Expected: the guest build succeeds; the host build fails because
`zb/library_runtime.h` does not exist and `zb_service_api` has no `malloc_fn`.

- [ ] **Step 2: Protocol version 2, zbhost preload and AGAIN loops, index reservation**

Write `core/include/zb/library_protocol.h` with exactly this content:

```c
#pragma once

/* Library-mode handshake between the host LibraryRuntime and guest/zbhost/zbhost.c.
 * C-compatible: the arm32 guest includes this header too. */

#include <stdint.h>

#define ZB_SERVICE_PROTOCOL_VERSION 2u

/* Host-call indices 0xFE00-0xFEFF belong to the library runtime; tools/gen_stubs.py keeps
 * generated stub indices below this range. */
#define ZB_RUNTIME_HOST_CALL_FIRST 0xFE00u
#define ZB_RUNTIME_HOST_CALL_LAST 0xFEFFu
#define ZB_SERVICE_READY_INDEX 0xFE00u
#define ZB_CARRIER_PARK_INDEX 0xFE01u

/* r0 from READY or PARK: return to guest code so pending signals are delivered, then make
 * the same host call again. */
#define ZB_SERVICE_AGAIN 0xFFFFFFF5u

#define ZB_SERVICE_SCRATCH_SIZE 4096u

/* zbhost exit statuses before READY. */
#define ZB_HOST_EXIT_USAGE 2
#define ZB_HOST_EXIT_TARGET_SDK 3
#define ZB_HOST_EXIT_PRELOAD 4

/* 32-bit bionic dlopen flags and handles. Host <dlfcn.h> values differ (host RTLD_NOW is 2,
 * RTLD_GLOBAL 0x100, RTLD_DEFAULT 0). */
#define ZB_GUEST_RTLD_NOW 0u
#define ZB_GUEST_RTLD_LAZY 1u
#define ZB_GUEST_RTLD_GLOBAL 2u
#define ZB_GUEST_RTLD_NOLOAD 4u
#define ZB_GUEST_RTLD_NODELETE 0x1000u
#define ZB_GUEST_RTLD_DEFAULT 0xffffffffu

struct zb_service_api {
    uint32_t size;
    uint32_t version;
    uint32_t dlopen_fn;        /* uint32_t (const char* path, uint32_t guest_flags) */
    uint32_t dlsym_fn;         /* uint32_t (uint32_t handle, const char* name) */
    uint32_t dlerror_fn;       /* uint32_t (void): const char* or 0, per guest thread */
    uint32_t spawn_carrier_fn; /* uint32_t (void): pthread_create result */
    uint32_t malloc_fn;        /* uint32_t (uint32_t size) */
    uint32_t free_fn;          /* void (uint32_t pointer) */
    uint32_t scratch;          /* service-thread string buffer */
    uint32_t scratch_size;
};
```

Write `guest/zbhost/zbhost.c` with exactly this content:

```c
/* Library-mode guest service. Usage: zbhost <target_sdk> [<preload>]
 * Sets the linker target SDK, preloads libzbcompat.so and the optional library with
 * RTLD_GLOBAL, publishes zb_service_api through READY, and then serves host requests inside
 * that host call. Carriers are guest pthreads that park in PARK until a host thread has
 * borrowed and released them. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zb/library_protocol.h"

_Static_assert(ZB_GUEST_RTLD_NOW == RTLD_NOW, "guest RTLD_NOW");
_Static_assert(ZB_GUEST_RTLD_LAZY == RTLD_LAZY, "guest RTLD_LAZY");
_Static_assert(ZB_GUEST_RTLD_GLOBAL == RTLD_GLOBAL, "guest RTLD_GLOBAL");
_Static_assert(ZB_GUEST_RTLD_NOLOAD == RTLD_NOLOAD, "guest RTLD_NOLOAD");
_Static_assert(ZB_GUEST_RTLD_NODELETE == RTLD_NODELETE, "guest RTLD_NODELETE");

static char scratch[ZB_SERVICE_SCRATCH_SIZE];

static uint32_t host_call(uint32_t index, uint32_t arg) {
    register uint32_t r0 __asm__("r0") = arg;
    if (index == ZB_SERVICE_READY_INDEX) {
        __asm__ volatile("svc #0x5afe00" : "+r"(r0) : : "memory");
    } else {
        __asm__ volatile("svc #0x5afe01" : "+r"(r0) : : "memory");
    }
    return r0;
}

static uint32_t service_dlopen(const char* path, uint32_t flags) {
    return (uint32_t)(uintptr_t)dlopen(path, (int)flags);
}

static uint32_t service_dlsym(uint32_t handle, const char* name) {
    return (uint32_t)(uintptr_t)dlsym((void*)(uintptr_t)handle, name);
}

static uint32_t service_dlerror(void) {
    return (uint32_t)(uintptr_t)dlerror();
}

static uint32_t service_malloc(uint32_t size) {
    return (uint32_t)(uintptr_t)malloc(size);
}

static void service_free(uint32_t pointer) {
    free((void*)(uintptr_t)pointer);
}

static void* carrier_main(void* unused) {
    (void)unused;
    while (host_call(ZB_CARRIER_PARK_INDEX, 0) == ZB_SERVICE_AGAIN) {
    }
    return NULL;
}

static uint32_t spawn_carrier(void) {
    pthread_t thread;
    const int rc = pthread_create(&thread, NULL, carrier_main, NULL);
    if (rc == 0) pthread_detach(thread);
    return (uint32_t)rc;
}

static int preload(const char* path) {
    if (dlopen(path, RTLD_NOW | RTLD_GLOBAL) != NULL) return 1;
    const char* error = dlerror();
    fprintf(stderr, "zbhost: cannot preload %s: %s\n", path, error != NULL ? error : "unknown error");
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return ZB_HOST_EXIT_USAGE;
    void* dl_android = dlopen("libdl_android.so", RTLD_NOW);
    void (*set_target_sdk)(unsigned) = dl_android != NULL
        ? (void (*)(unsigned))dlsym(dl_android, "android_set_application_target_sdk_version")
        : NULL;
    if (set_target_sdk == NULL) return ZB_HOST_EXIT_TARGET_SDK;
    set_target_sdk((unsigned)strtoul(argv[1], NULL, 10));
    if (!preload("libzbcompat.so")) return ZB_HOST_EXIT_PRELOAD;
    if (argc == 3 && !preload(argv[2])) return ZB_HOST_EXIT_PRELOAD;
    const struct zb_service_api api = {
        sizeof(api), ZB_SERVICE_PROTOCOL_VERSION,
        (uint32_t)(uintptr_t)service_dlopen,
        (uint32_t)(uintptr_t)service_dlsym,
        (uint32_t)(uintptr_t)service_dlerror,
        (uint32_t)(uintptr_t)spawn_carrier,
        (uint32_t)(uintptr_t)service_malloc,
        (uint32_t)(uintptr_t)service_free,
        (uint32_t)(uintptr_t)scratch, sizeof(scratch),
    };
    uint32_t status;
    do {
        status = host_call(ZB_SERVICE_READY_INDEX, (uint32_t)(uintptr_t)&api);
    } while (status == ZB_SERVICE_AGAIN);
    return (int)status;
}
```

In `tools/gen_stubs.py`, replace:

```python
them. Index 0xFFFF is reserved for returning from host->guest calls.
```

with:

```python
them. Indices 0xFE00-0xFEFF are reserved for the library runtime
(core/include/zb/library_protocol.h) and 0xFFFF for returning from host->guest calls, so
generated indices must stay below 0xFE00.
```

In `tools/gen_stubs.py`, replace:

```python
HOST_RETURN_INDEX = 0xFFFF
```

with:

```python
HOST_RETURN_INDEX = 0xFFFF
# ZB_RUNTIME_HOST_CALL_FIRST..LAST in core/include/zb/library_protocol.h.
RUNTIME_HOST_CALL_FIRST = 0xFE00
```

In `tools/gen_stubs.py`, replace:

```python
            if index >= HOST_RETURN_INDEX:
                sys.exit("too many host calls")
```

with:

```python
            if index >= RUNTIME_HOST_CALL_FIRST:
                sys.exit("too many host calls: index 0x%x reaches the runtime range" % index)
```

```sh
python3 tools/gen_stubs.py; git status --short guest/stubs core/src/gen; tools/build_guest.sh; ninja -C build/host zbhost_protocol_test; ctest --test-dir build/host -R '^zbhost_protocol_test$' --output-on-failure
```

Expected: `host calls: 160`, no changed generated files, the zbhost `_Static_assert`s on
the RTLD values compile, and `zbhost_protocol_test` passes (it also checks that a missing
preload exits with status 4).

- [ ] **Step 3: Signal-interruptible parking and `Process::current_thread`**

In `core/include/zb/guest_thread.h`, replace:

```cpp
    // Queues a signal for this thread and interrupts its JIT. Async-signal-safe.
    void post_signal(const g::siginfo32& info);
```

with:

```cpp
    // Queues a signal for this thread, interrupts its JIT and wakes park(). Async-signal-safe.
    void post_signal(const g::siginfo32& info);
```

In `core/include/zb/guest_thread.h`, replace:

```cpp
    std::uint64_t pending_signals() const { return pending_signals_.load(); }
```

with:

```cpp
    std::uint64_t pending_signals() const { return pending_signals_.load(); }

    // Parking for a thread that waits inside a host call (library runtime service and carriers).
    // wake() and post_signal() change the token; park(token) sleeps only while the token is
    // unchanged, so a waiter that reads the token before checking its condition loses no wakeup.
    // park() may return spuriously (host signal, EINTR).
    std::uint32_t park_token() const { return park_word_.load(); }
    void park(std::uint32_t token);
    // Async-signal-safe.
    void wake();
```

In `core/include/zb/guest_thread.h`, replace:

```cpp
    std::array<g::siginfo32, 65> pending_info_{};
};
```

with:

```cpp
    std::array<g::siginfo32, 65> pending_info_{};
    // futex word; std::atomic<std::uint32_t> has the layout of std::uint32_t.
    std::atomic<std::uint32_t> park_word_{0};
};
```

In `core/src/guest_thread.cpp`, replace:

```cpp
#include <bit>
#include <cstring>
```

with:

```cpp
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bit>
#include <climits>
#include <cstring>
```

In `core/src/guest_thread.cpp`, replace:

```cpp
    pending_signals_.fetch_or(1ULL << (sig - 1));
    jit_->HaltExecution(kInterruptHalt);
}
```

with:

```cpp
    pending_signals_.fetch_or(1ULL << (sig - 1));
    jit_->HaltExecution(kInterruptHalt);
    wake();
}

void GuestThread::park(std::uint32_t token) {
    ::syscall(SYS_futex, &park_word_, FUTEX_WAIT_PRIVATE, token, nullptr, nullptr, 0);
}

void GuestThread::wake() {
    park_word_.fetch_add(1);
    ::syscall(SYS_futex, &park_word_, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
```

In `core/include/zb/process.h`, replace:

```cpp
    static void set_current_thread(GuestThread* thread);
```

with:

```cpp
    static void set_current_thread(GuestThread* thread);
    // Guest thread running on the calling host thread, or nullptr.
    static GuestThread* current_thread();
```

In `core/src/signals.cpp`, replace:

```cpp
void Process::set_current_thread(GuestThread* thread) {
    t_current_thread = thread;
}
```

with:

```cpp
void Process::set_current_thread(GuestThread* thread) {
    t_current_thread = thread;
}

GuestThread* Process::current_thread() {
    return t_current_thread;
}
```

```sh
ninja -C build/host guest_call_test host_call_dispatch_test async_signal_test; ctest --test-dir build/host -R '^(guest_call_test|host_call_dispatch_test|async_signal_test)$' --output-on-failure
```

Expected: PASS; parking is not used yet, and waking a thread that is not parked is
harmless.

- [ ] **Step 4: Implement the service-thread runtime**

Write `core/include/zb/library_runtime.h` with exactly this content:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_protocol.h"
#include "zb/native_call.h"
#include "zb/process.h"

namespace zb {

struct LibraryRuntimeOptions {
    std::string zbhost;   // host path of the arm32 zbhost executable
    std::string sysroot;  // host directory with the arm32 system files
    std::uint32_t target_sdk = 0;
    std::vector<std::string> envp;  // guest environment, e.g. LD_LIBRARY_PATH=...
    std::string preload;            // guest path dlopen'ed RTLD_GLOBAL before READY; empty: none
    std::chrono::milliseconds ready_timeout{10000};
};

// A guest process in library mode: zbhost runs on a service guest thread that owns the boot
// JIT and serves requests from inside its READY host call.
//
// The runtime is process-lifetime. Guest threads cannot be torn down, so after a successful
// start() the destructor logs and aborts; tests end with std::_Exit.
class LibraryRuntime {
public:
    LibraryRuntime();
    ~LibraryRuntime();
    LibraryRuntime(const LibraryRuntime&) = delete;
    LibraryRuntime& operator=(const LibraryRuntime&) = delete;

    // Receives every host call outside the runtime range 0xFE00-0xFEFF. Call before start().
    void set_host_call_handler(Process::HostCallHandler handler);
    bool start(const LibraryRuntimeOptions& options, std::string& error);

    // Guest dlopen/dlsym/dlerror on the service thread. Flags are ZB_GUEST_RTLD_* values.
    // Return 0 and set error (guest dlerror text) on failure.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::optional<GuestResult> call_on_service(std::uint32_t function, const GuestCall& args);
    // Nested call on the guest thread the calling host thread already runs (for host-call
    // handlers). nullopt if the calling host thread runs no guest code.
    std::optional<GuestResult> call_on_current(std::uint32_t function, const GuestCall& args);

    GuestMemory& memory();
    // Valid after a successful start().
    const zb_service_api& service_api() const;
    // Real guest pthreads (service, carriers, guest-created threads); borrowers are not counted.
    std::size_t guest_thread_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
```

Write `core/src/library_runtime.cpp` with exactly this content:

```cpp
#include "zb/library_runtime.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "zb/log.h"

namespace zb {

namespace {

struct Response {
    bool ok = false;
    std::optional<GuestResult> result;
    std::string error;
};

struct Command {
    enum class Kind { Load, Symbol, Call };
    Kind kind = Kind::Call;
    std::uint32_t value = 0;
    std::uint32_t flags = 0;
    std::string text;
    GuestCall args;
    std::promise<Response> done;
};

using Invoke = std::function<std::optional<GuestResult>(std::uint32_t function, const GuestCall& args)>;

std::string exit_message(int status) {
    if (status == ZB_HOST_EXIT_PRELOAD) return "zbhost could not preload a library (status 4)";
    return "zbhost exited with status " + std::to_string(status);
}

}  // namespace

struct LibraryRuntime::Impl {
    Process process;
    Process::HostCallHandler chained;
    std::thread runner;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Command>> commands;
    zb_service_api api{};
    GuestThread* service = nullptr;
    std::thread::id service_id;
    bool started = false;
    bool ready = false;
    bool finished = false;
    std::string startup_error;

    bool validate_api(std::uint32_t address, std::string& error) {
        const std::uint8_t* source = process.memory().host_ptr(address, sizeof api, kPageRead);
        if (source == nullptr) {
            error = "zbhost passed an unreadable service API";
            return false;
        }
        zb_service_api candidate;
        std::memcpy(&candidate, source, sizeof candidate);
        if (candidate.size != sizeof candidate || candidate.version != ZB_SERVICE_PROTOCOL_VERSION) {
            error = "zbhost service protocol mismatch";
            return false;
        }
        if (candidate.dlopen_fn == 0 || candidate.dlsym_fn == 0 || candidate.dlerror_fn == 0 ||
            candidate.spawn_carrier_fn == 0 || candidate.malloc_fn == 0 || candidate.free_fn == 0 ||
            candidate.scratch_size == 0 || candidate.scratch_size > ZB_SERVICE_SCRATCH_SIZE ||
            process.memory().host_ptr(candidate.scratch, candidate.scratch_size, kPageWrite) == nullptr) {
            error = "zbhost passed an invalid service API";
            return false;
        }
        api = candidate;
        return true;
    }

    bool copy_text(std::uint32_t buffer, std::uint32_t size, const std::string& text, std::string& error) {
        if (text.find('\0') != std::string::npos || text.size() + 1 > size) {
            error = "guest loader string does not fit its scratch buffer";
            return false;
        }
        std::uint8_t* destination = process.memory().host_ptr(buffer, text.size() + 1, kPageWrite);
        if (destination == nullptr) {
            error = "guest loader scratch buffer is not writable";
            return false;
        }
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    }

    std::string read_text(std::uint32_t address) {
        if (address == 0) return "guest dlerror returned null";
        std::string text;
        for (std::uint32_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
            const std::uint8_t* byte = process.memory().host_ptr(address + i, 1, kPageRead);
            if (byte == nullptr) return "guest dlerror returned an unreadable string";
            if (*byte == 0) return text;
            text.push_back(static_cast<char>(*byte));
        }
        return "guest dlerror string is not terminated";
    }

    // dlerror is per guest thread: read it through the same invoke as the failed call.
    std::string last_dlerror(const Invoke& invoke) {
        const auto result = invoke(api.dlerror_fn, GuestCall{});
        return result ? read_text(result->r0) : "guest dlerror call failed";
    }

    std::uint32_t load(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, const std::string& path,
                       std::uint32_t guest_flags, std::string& error) {
        if (!copy_text(buffer, size, path, error)) return 0;
        GuestCall args;
        args.regs = {buffer, guest_flags, 0, 0};
        const auto result = invoke(api.dlopen_fn, args);
        if (!result) {
            error = "guest dlopen call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    std::uint32_t symbol(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, std::uint32_t handle,
                         const std::string& name, std::string& error) {
        if (!copy_text(buffer, size, name, error)) return 0;
        GuestCall args;
        args.regs = {handle, buffer, 0, 0};
        const auto result = invoke(api.dlsym_fn, args);
        if (!result) {
            error = "guest dlsym call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    Response execute(GuestThread& thread, const Command& command) {
        const Invoke invoke = [&](std::uint32_t function, const GuestCall& args) {
            return process.call_guest(thread, function, args);
        };
        Response response;
        switch (command.kind) {
        case Command::Kind::Load:
            response.result = GuestResult{
                load(invoke, api.scratch, api.scratch_size, command.text, command.flags, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Symbol:
            response.result = GuestResult{
                symbol(invoke, api.scratch, api.scratch_size, command.value, command.text, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Call:
            response.result = invoke(command.value, command.args);
            response.ok = response.result.has_value();
            if (!response.ok) response.error = "guest service call failed";
            break;
        }
        return response;
    }

    // Serves commands until a signal must be delivered (r0 = AGAIN) or the guest is exiting.
    void serve(GuestThread& thread) {
        for (;;) {
            const std::uint32_t token = thread.park_token();
            if (thread.has_pending_signals(thread.sigmask)) {
                thread.regs()[0] = ZB_SERVICE_AGAIN;
                return;
            }
            std::shared_ptr<Command> command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    command = commands.front();
                    commands.pop_front();
                }
            }
            if (!command) {
                thread.park(token);
                continue;
            }
            command->done.set_value(execute(thread, *command));
            if (process.exiting()) {
                thread.regs()[0] = 1;
                return;
            }
        }
    }

    bool handle_ready(GuestThread& thread) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!ready) {
                std::string error;
                if (!validate_api(thread.regs()[0], error)) {
                    startup_error = std::move(error);
                    lock.unlock();
                    cv.notify_all();
                    thread.regs()[0] = 1;
                    return true;
                }
                service = &thread;
                service_id = std::this_thread::get_id();
                ready = true;
                lock.unlock();
                cv.notify_all();
            }
        }
        serve(thread);
        return true;
    }

    bool handle_host_call(std::uint32_t index, GuestThread& thread) {
        if (index >= ZB_RUNTIME_HOST_CALL_FIRST && index <= ZB_RUNTIME_HOST_CALL_LAST) {
            if (index == ZB_SERVICE_READY_INDEX) return handle_ready(thread);
            return false;
        }
        return chained && chained(index, thread);
    }

    Response submit(std::shared_ptr<Command> command) {
        std::future<Response> future = command->done.get_future();
        GuestThread* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || finished) return {false, std::nullopt, "guest library runtime is not running"};
            if (std::this_thread::get_id() == service_id) {
                return {false, std::nullopt, "guest service request made on the service thread; use call_on_current"};
            }
            commands.push_back(std::move(command));
            target = service;
        }
        target->wake();
        return future.get();
    }
};

LibraryRuntime::LibraryRuntime() : impl_(std::make_unique<Impl>()) {}

LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready || !impl_->finished) {
            log("LibraryRuntime destroyed after start; the runtime is process-lifetime");
            std::abort();
        }
    }
    impl_->runner.join();
}

void LibraryRuntime::set_host_call_handler(Process::HostCallHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) {
        log("LibraryRuntime::set_host_call_handler called after start");
        std::abort();
    }
    impl_->chained = std::move(handler);
}

bool LibraryRuntime::start(const LibraryRuntimeOptions& options, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) {
            error = "guest library runtime was already started";
            return false;
        }
        impl_->started = true;
    }
    Impl* impl = impl_.get();
    impl->process.set_sysroot(options.sysroot);
    impl->process.set_host_call_handler(
        [impl](std::uint32_t index, GuestThread& thread) { return impl->handle_host_call(index, thread); });
    std::vector<std::string> argv = {options.zbhost, std::to_string(options.target_sdk)};
    if (!options.preload.empty()) argv.push_back(options.preload);
    impl->runner = std::thread([impl, argv, options] {
        const int status = impl->process.run(options.zbhost, argv, options.envp);
        std::deque<std::shared_ptr<Command>> pending;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->finished = true;
            if (!impl->ready && impl->startup_error.empty()) impl->startup_error = exit_message(status);
            pending.swap(impl->commands);
        }
        impl->cv.notify_all();
        for (const auto& command : pending) command->done.set_value({false, std::nullopt, exit_message(status)});
    });

    std::unique_lock<std::mutex> lock(impl->mutex);
    const bool settled = impl->cv.wait_for(lock, options.ready_timeout, [impl] {
        return impl->ready || impl->finished || !impl->startup_error.empty();
    });
    if (impl->ready) return true;
    error = settled ? impl->startup_error
                    : "zbhost did not report ready within " + std::to_string(options.ready_timeout.count()) + " ms";
    return false;
}

std::uint32_t LibraryRuntime::load_library(const std::string& path, std::uint32_t guest_flags, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Load;
    command->text = path;
    command->flags = guest_flags;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::uint32_t LibraryRuntime::find_symbol(std::uint32_t handle, const std::string& name, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Symbol;
    command->value = handle;
    command->text = name;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::optional<GuestResult> LibraryRuntime::call_on_service(std::uint32_t function, const GuestCall& args) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Call;
    command->value = function;
    command->args = args;
    Response response = impl_->submit(std::move(command));
    return response.ok ? response.result : std::nullopt;
}

std::optional<GuestResult> LibraryRuntime::call_on_current(std::uint32_t function, const GuestCall& args) {
    GuestThread* thread = Process::current_thread();
    if (thread == nullptr) return std::nullopt;
    return impl_->process.call_guest(*thread, function, args);
}

GuestMemory& LibraryRuntime::memory() {
    return impl_->process.memory();
}

const zb_service_api& LibraryRuntime::service_api() const {
    return impl_->api;
}

std::size_t LibraryRuntime::guest_thread_count() const {
    return impl_->process.thread_count();
}

}  // namespace zb
```

In `core/CMakeLists.txt`, replace:

```cmake
    src/jni/thunks.S
```

with:

```cmake
    src/jni/thunks.S
    src/library_runtime.cpp
```

```sh
ninja -C build/host library_runtime_test; ctest --test-dir build/host -R '^library_runtime_test$' --output-on-failure
```

Expected output ends with:

```text
expected start error: zbhost could not preload a library (status 4)
library_runtime_test PASS
```

- [ ] **Step 5: Show that the futex wake is load-bearing**

Temporarily delete the `wake();` line from `GuestThread::post_signal`, then run:

```sh
ninja -C build/host library_runtime_test; build/host/tests/host/library_runtime_test sysroot build/guest/zbhost build/guest/lib/libzbcallprobe.so
```

Expected: `CHECK failed: wait_nonzero(runtime, alarm_count, 2000ms)`. Restore the line
and rebuild before continuing.

- [ ] **Step 6: Verify and commit**

```sh
tools/build_guest.sh; ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/run_guest_tests.sh; git status --short
```

Expected: 14/14 host tests; guest cases pass.

```sh
git add core/include/zb/library_runtime.h core/src/library_runtime.cpp core/include/zb/library_protocol.h guest/zbhost/zbhost.c guest/testlib/zbcallprobe.c core/include/zb/guest_thread.h core/src/guest_thread.cpp core/include/zb/process.h core/src/signals.cpp core/CMakeLists.txt tests/host/CMakeLists.txt tests/host/library_runtime_test.cpp tests/host/zbhost_protocol_test.cpp tools/build_guest.sh tools/gen_stubs.py; git commit -m "core: load and call guest libraries through zbhost"
```

---

## Task 5: Carrier leases, tid routing, and state inheritance

**Status:** ready, plan revised after review (D5, D6, D7, D9, D10, D12, D14).

**Files:**
- Modify: `core/include/zb/guest_thread.h`
- Modify: `core/include/zb/process.h`, `core/src/process.cpp`
- Modify: `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp`
- Modify: `tests/host/host_call_dispatch_test.cpp`, `tests/host/library_runtime_test.cpp`

**Interfaces:** `Process::create_borrower(GuestThread& carrier)`,
`Process::destroy_borrower(std::unique_ptr<GuestThread>, GuestThread& carrier)`,
`GuestThread::child_code_cache_size`, `LibraryRuntime::borrow`,
`LibraryRuntime::Carrier::{call, load_library, find_symbol, guest_tid, guest_tls}`.

Design notes:

- **Borrowing.** `borrow` submits `SpawnCarrier` (guest `pthread_create`, serialized
  through the service thread, with a 2 MiB carrier JIT), then waits up to 10 s for any
  carrier listed as available, marks it leased under the runtime mutex, and creates the
  borrower JIT on the calling host thread.
- **Carrier states.** A carrier record is `Available` only while its thread is inside
  `PARK`. While available, pending unblocked signals make `PARK` return
  `ZB_SERVICE_AGAIN` (the record is unlisted first). A leased carrier never leaves
  `PARK`; its host thread clears `Process::current_thread` so host signals landing there go
  to the process signal target. `Released` makes `PARK` return 0, and the thread exits
  through bionic.
- **Inheritance (D7).** `create_borrower` copies TLS, tid, `sp & ~7`, sigmask, altstack
  and FPSCR from the parked carrier. `destroy_borrower` copies sigmask and altstack back and
  moves still-pending borrower signals to the carrier.
- **Routing.** `find_thread` searches borrowers first, so `tkill`/`tgkill` aimed at the
  carrier tid reach the borrower. `invalidate` covers borrowers. `thread_count` does not.
- **Release.** The lease destructor frees the carrier's guest scratch buffer, destroys the
  borrower, and marks the record released and wakes the carrier under the mutex (after
  that the carrier may exit and free its `GuestThread`).
- **Loader on a carrier (D9).** `Carrier::load_library`/`find_symbol` allocate a
  `ZB_SERVICE_SCRATCH_SIZE` buffer with guest `malloc` on first use and read `dlerror` on
  the same carrier.
- **Misuse (D10).** `borrow` fails when `Process::current_thread()` is not null (service
  thread, a borrower inside a host call, a guest pthread); callers use `call_on_current`.
  A lease used or released on another host thread aborts.
- **Limits.** Each lease costs two processor ids and two JITs, and the borrower
  translates cold. PI futexes are not supported on borrowers.

- [ ] **Step 1: Add the failing tests**

`host_call_dispatch_test` creates a borrower on a second host thread from the main guest
thread while it is inside host call `0xFE10`, checks inheritance and routing, changes the
borrower's mask and altstack, leaves a blocked signal pending, and checks the copy-back.

Write `tests/host/host_call_dispatch_test.cpp` with exactly this content:

```cpp
// Process host-call dispatch, plus borrowed JITs: a borrower created from a guest thread parked
// in a host call inherits its identity and emulated state, receives tid-directed signals, and
// hands its signal mask, alternate stack and pending signals back when destroyed.
// Usage: host_call_dispatch_test <build/guest/host_call_static>
#include <signal.h>

#include <string>
#include <thread>

#include "check.h"
#include "zb/process.h"

namespace {

std::uint64_t sig_bit(int sig) {
    return 1ULL << (sig - 1);
}

void check_borrower(zb::Process& process, zb::GuestThread& carrier) {
    const std::uint64_t saved_mask = carrier.sigmask;
    const zb::g::stack32 saved_altstack = carrier.altstack;
    const std::uint32_t saved_fpscr = carrier.fpscr();
    carrier.sigmask = sig_bit(SIGUSR2);
    carrier.altstack = {0x50000, 0, 0x4000};
    carrier.set_fpscr(0x00C00000);

    std::thread([&] {
        std::unique_ptr<zb::GuestThread> borrower = process.create_borrower(carrier);
        CHECK(borrower != nullptr);
        CHECK(borrower->processor_id() != carrier.processor_id());
        CHECK(borrower->tid == carrier.tid && borrower->tls() == carrier.tls());
        CHECK(borrower->regs()[13] == (carrier.regs()[13] & ~7u));
        CHECK(borrower->sigmask == sig_bit(SIGUSR2) && borrower->fpscr() == 0x00C00000);
        CHECK(borrower->altstack.ss_sp == 0x50000 && borrower->altstack.ss_size == 0x4000);
        CHECK(process.find_thread(carrier.tid) == borrower.get());
        CHECK(process.thread_count() == 1);
        process.invalidate(0x10000, 0x1000);

        borrower->sigmask = sig_bit(SIGUSR1);
        borrower->altstack = {0x60000, 0, 0x8000};
        zb::g::siginfo32 info{};
        info.si_signo = SIGUSR1;
        borrower->post_signal(info);
        process.destroy_borrower(std::move(borrower), carrier);
    }).join();

    CHECK(carrier.sigmask == sig_bit(SIGUSR1));
    CHECK(carrier.altstack.ss_sp == 0x60000 && carrier.altstack.ss_size == 0x8000);
    CHECK(carrier.pending_signals() == sig_bit(SIGUSR1));
    CHECK(process.find_thread(carrier.tid) == &carrier);

    zb::g::siginfo32 moved;
    CHECK(carrier.take_signal(0, moved) && moved.si_signo == SIGUSR1);
    carrier.sigmask = saved_mask;
    carrier.altstack = saved_altstack;
    carrier.set_fpscr(saved_fpscr);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    zb::Process process;
    bool called = false;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFE10) return false;
        CHECK(thread.regs()[0] == 7);
        check_borrower(process, thread);
        thread.regs()[0] = 49;
        called = true;
        return true;
    });
    CHECK(process.run(argv[1], {argv[1]}, {}) == 0);
    CHECK(called);
    std::puts("host_call_dispatch_test PASS");
    return 0;
}
```

`library_runtime_test` keeps the Task 4 checks and adds: first-borrow latency, carrier
identity (`gettid` syscall, cached tid, TLS), loader on a carrier, every return type and
the mixed signature on a carrier, chained host calls and the borrow misuse guard on a
borrower, tgkill redirection, `setitimer` from a borrower delivered to the parked
service, sigmask and altstack inheritance, release settling `guest_thread_count`, two
concurrent borrowers under mutex/malloc/errno contention, and reuse.

Write `tests/host/library_runtime_test.cpp` with exactly this content:

```cpp
// Library-mode runtime: guest RTLD flags, dlopen/dlsym/dlerror on the service thread and on a
// carrier, every return type, host-call chaining, misuse guards, preload failure, signals to
// parked threads, carrier identity and state inheritance, tgkill redirection, concurrent
// borrowers under bionic contention, and carrier cleanup.
// Usage: library_runtime_test <sysroot> <zbhost> <libzbcallprobe.so>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "check.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using GuestInvoke = std::function<std::optional<zb::GuestResult>(std::uint32_t function, const zb::GuestCall& args)>;

std::uint32_t fbits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t dbits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

double elapsed_ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

zb::GuestCall args1(std::uint32_t r0) {
    zb::GuestCall call;
    call.regs = {r0, 0, 0, 0};
    return call;
}

std::uint32_t read32(zb::LibraryRuntime& runtime, std::uint32_t address) {
    const std::uint8_t* source = runtime.memory().host_ptr(address, 4, zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

// Polls a guest word without running guest code on this host thread.
bool wait_nonzero(zb::LibraryRuntime& runtime, std::uint32_t address, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (read32(runtime, address) == 0) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Released carriers exit through bionic asynchronously.
bool wait_thread_count(zb::LibraryRuntime& runtime, std::size_t expected, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (runtime.guest_thread_count() != expected) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

zb::LibraryRuntimeOptions runtime_options(char** argv) {
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.envp = {"LD_LIBRARY_PATH=" + std::filesystem::path(argv[3]).parent_path().string()};
    return options;
}

// start() reports a failed preload; runs in a child because a runtime is process-lifetime.
void check_preload_failure(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        zb::LibraryRuntime runtime;
        zb::LibraryRuntimeOptions options = runtime_options(argv);
        options.preload = "libzb-does-not-exist.so";
        std::string error;
        if (runtime.start(options, error)) std::_Exit(10);
        std::fprintf(stderr, "expected start error: %s\n", error.c_str());
        std::_Exit(error.find("status 4") != std::string::npos ? 0 : 11);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Every JNI return type through store_native_result.
void check_returns(const GuestInvoke& invoke, const std::function<std::uint32_t(const char*)>& symbol) {
    struct ReturnCase {
        const char* name;
        char type;
        std::uint64_t x0;
        std::uint64_t d0;
    };
    const ReturnCase returns[] = {
        {"zb_return_z", 'Z', 1, 0},
        {"zb_return_b", 'B', static_cast<std::uint64_t>(-2), 0},
        {"zb_return_c", 'C', 0x1234, 0},
        {"zb_return_s", 'S', static_cast<std::uint64_t>(-3), 0},
        {"zb_return_i", 'I', 42, 0},
        {"zb_return_j", 'J', 0x1122334455667788ull, 0},
        {"zb_return_f", 'F', 0, fbits(3.5f)},
        {"zb_return_d", 'D', 0, dbits(-1.25)},
        {"zb_return_l", 'L', 0x7000012345ull, 0},
    };
    CHECK(invoke(symbol("zb_return_v"), zb::GuestCall{}));
    for (const ReturnCase& expected : returns) {
        const auto result = invoke(symbol(expected.name), zb::GuestCall{});
        CHECK(result);
        zb::NativeRegs regs{};
        zb::store_native_result(expected.type, result->r0, result->r1, regs,
                                [](std::uint32_t handle) { return 0x7000000000ull | handle; });
        if (expected.type == 'F' || expected.type == 'D') {
            CHECK(regs.d[0] == expected.d0);
        } else {
            CHECK(regs.x[0] == expected.x0);
        }
    }
}

// softfp argument layout of a mixed JNI signature.
void check_mixed_arguments(const GuestInvoke& invoke, std::uint32_t mix) {
    std::uint64_t host_stack[1] = {0x99};
    zb::NativeRegs host{};
    host.x[1] = 0x77;
    host.x[2] = 1;
    host.x[3] = static_cast<std::uint64_t>(-2);
    host.x[4] = 0x1234;
    host.x[5] = static_cast<std::uint64_t>(-3);
    host.x[6] = 4;
    host.x[7] = 0x1122334455667788ull;
    host.d[0] = fbits(1.5f);
    host.d[1] = dbits(-2.25);
    host.stack = host_stack;
    const auto to_handle = [](std::uint64_t reference) {
        return reference == 0 ? 0u : static_cast<std::uint32_t>(reference + 0x1000);
    };
    const zb::GuestCall mixed = zb::marshal_native_args("IZBCSIJFDL", host, 0xE000, to_handle);
    const auto result = invoke(mix, mixed);
    CHECK(result && result->r0 == 42);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string probe = argv[3];
    const std::string libdir = std::filesystem::path(probe).parent_path();
    check_preload_failure(argv);

    CHECK(ZB_GUEST_RTLD_NOW == 0u && ZB_GUEST_RTLD_LAZY == 1u && ZB_GUEST_RTLD_GLOBAL == 2u);
    CHECK(ZB_GUEST_RTLD_NOLOAD == 4u && ZB_GUEST_RTLD_NODELETE == 0x1000u && ZB_GUEST_RTLD_DEFAULT == 0xFFFFFFFFu);

    zb::LibraryRuntime runtime;
    std::atomic<std::uint32_t> chained_function{0};
    std::atomic<int> chained_calls{0};
    std::atomic<bool> chained_on_service{true};
    runtime.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFD00) return false;
        ++chained_calls;
        std::string misuse;
        CHECK(!runtime.borrow(misuse));
        CHECK(misuse.find("call_on_current") != std::string::npos);
        if (chained_on_service) {
            misuse.clear();
            CHECK(runtime.load_library(argv[3], ZB_GUEST_RTLD_NOW, misuse) == 0);
            CHECK(misuse.find("service thread") != std::string::npos);
        }
        const auto nested = runtime.call_on_current(chained_function, zb::GuestCall{});
        CHECK(nested && nested->r0 == 42);
        thread.regs()[0] += nested->r0;
        return true;
    });

    std::string error;
    CHECK(runtime.start(runtime_options(argv), error));
    CHECK(runtime.guest_thread_count() == 1);
    CHECK(runtime.service_api().version == ZB_SERVICE_PROTOCOL_VERSION);
    CHECK(!runtime.call_on_current(runtime.service_api().malloc_fn, zb::GuestCall{}));

    // Loader on the service thread.
    CHECK(runtime.load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == 0);
    error.clear();
    const std::uint32_t library = runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library);
    CHECK(runtime.find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    error.clear();
    CHECK(runtime.find_symbol(ZB_GUEST_RTLD_DEFAULT, "zb_return_i", error) == symbol("zb_return_i"));

    const GuestInvoke on_service = [&](std::uint32_t function, const zb::GuestCall& args) {
        return runtime.call_on_service(function, args);
    };
    check_returns(on_service, symbol);
    check_mixed_arguments(on_service, symbol("zb_probe_mix"));

    // Host calls outside the runtime range reach the chained handler, which may nest a call.
    chained_function = symbol("zb_return_i");
    const std::uint32_t host_call = symbol("zb_probe_host_call");
    const auto chained = runtime.call_on_service(host_call, args1(100));
    CHECK(chained && chained->r0 == 142 && chained_calls == 1);

    // A process-directed SIGALRM reaches the service thread while it is parked in READY.
    const std::uint32_t alarm_count = symbol("zb_alarm_count");
    const std::uint32_t alarm_tid = symbol("zb_alarm_tid");
    const std::uint32_t arm_alarm = symbol("zb_probe_arm_alarm");
    const std::uint32_t tid_function = symbol("zb_probe_tid");
    const std::uint32_t tls_function = symbol("zb_probe_tls");
    const auto service_tid = runtime.call_on_service(tid_function, zb::GuestCall{});
    const auto service_tls = runtime.call_on_service(tls_function, zb::GuestCall{});
    CHECK(service_tid && service_tid->r0 != 0 && service_tls && service_tls->r0 != 0);
    const auto armed = runtime.call_on_service(arm_alarm, args1(20000));
    CHECK(armed && armed->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);

    // First borrow: latency, identity, loader, returns and arguments on the carrier.
    const std::uint32_t return_i = symbol("zb_return_i");
    const auto borrow_start = Clock::now();
    auto first = runtime.borrow(error);
    CHECK(first && error.empty());
    const auto borrowed = Clock::now();
    const auto cold = first->call(return_i, zb::GuestCall{});
    const auto cold_done = Clock::now();
    const auto warm = first->call(return_i, zb::GuestCall{});
    const auto warm_done = Clock::now();
    CHECK(cold && cold->r0 == 42 && warm && warm->r0 == 42);
    std::printf("carrier latency: borrow %.2f ms, first call %.2f ms, warm call %.3f ms\n",
                elapsed_ms(borrow_start, borrowed), elapsed_ms(borrowed, cold_done), elapsed_ms(cold_done, warm_done));
    CHECK(runtime.guest_thread_count() == 2);

    const auto tid = first->call(tid_function, zb::GuestCall{});
    const auto cached_tid = first->call(symbol("zb_probe_cached_tid"), zb::GuestCall{});
    const auto tls = first->call(tls_function, zb::GuestCall{});
    CHECK(tid && cached_tid && tls);
    CHECK(static_cast<std::int32_t>(tid->r0) == first->guest_tid() && cached_tid->r0 == tid->r0);
    CHECK(tls->r0 == first->guest_tls() && tls->r0 != service_tls->r0 && tid->r0 != service_tid->r0);

    error.clear();
    CHECK(first->load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(first->load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library && error.empty());
    CHECK(first->find_symbol(library, "zb_return_i", error) == return_i && error.empty());
    CHECK(first->find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const GuestInvoke on_first = [&](std::uint32_t function, const zb::GuestCall& args) {
        return first->call(function, args);
    };
    check_returns(on_first, symbol);
    check_mixed_arguments(on_first, symbol("zb_probe_mix"));

    chained_on_service = false;
    const auto chained_on_carrier = first->call(host_call, args1(200));
    CHECK(chained_on_carrier && chained_on_carrier->r0 == 242 && chained_calls == 2);

    // tgkill to the borrowed carrier's tid is delivered on the borrower during the call.
    const auto installed = first->call(symbol("zb_probe_install_usr1"), zb::GuestCall{});
    CHECK(installed && installed->r0 == 0);
    const auto killed = first->call(symbol("zb_probe_tgkill_self"), args1(SIGUSR1));
    CHECK(killed && killed->r0 == 0);
    CHECK(read32(runtime, symbol("zb_usr1_count")) == 1);
    CHECK(read32(runtime, symbol("zb_usr1_tid")) == tid->r0);
    CHECK(read32(runtime, symbol("zb_usr1_tls")) == tls->r0);

    // setitimer from a borrower: SIGALRM is process-directed and reaches the parked service.
    const auto armed_on_carrier = first->call(arm_alarm, args1(20000));
    CHECK(armed_on_carrier && armed_on_carrier->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);

    // A carrier spawned from the service thread inherits its signal mask (clone), and the
    // borrower inherits the carrier's mask and bionic's alternate signal stack.
    const std::uint32_t block_signal = symbol("zb_probe_block_signal");
    const std::uint32_t signal_blocked = symbol("zb_probe_signal_blocked");
    zb::GuestCall block_usr2;
    block_usr2.regs = {static_cast<std::uint32_t>(SIGUSR2), 1, 0, 0};
    CHECK(runtime.call_on_service(block_signal, block_usr2));
    auto inherited = runtime.borrow(error);
    CHECK(inherited);
    const auto blocked = inherited->call(signal_blocked, args1(SIGUSR2));
    const auto altstack = inherited->call(symbol("zb_probe_altstack_sp"), zb::GuestCall{});
    CHECK(blocked && blocked->r0 == 1);
    CHECK(altstack && altstack->r0 != 0);
    block_usr2.regs[1] = 0;
    CHECK(runtime.call_on_service(block_signal, block_usr2));
    const auto service_blocked = runtime.call_on_service(signal_blocked, args1(SIGUSR2));
    CHECK(service_blocked && service_blocked->r0 == 0);
    CHECK(runtime.guest_thread_count() == 3);

    // Released carriers leave PARK and exit.
    inherited.reset();
    first.reset();
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    // Two host threads borrow at once and run bionic code concurrently.
    const std::uint32_t overlap = symbol("zb_probe_overlap");
    const std::uint32_t contention = symbol("zb_probe_contention");
    constexpr std::uint32_t kIterations = 20000;
    std::barrier rendezvous(2);
    std::array<std::uint32_t, 2> worker_tls{};
    std::array<std::uint32_t, 2> worker_tids{};
    std::array<std::uint32_t, 2> tickets{};
    const auto worker = [&](std::size_t index) {
        std::string borrow_error;
        auto carrier = runtime.borrow(borrow_error);
        CHECK(carrier && borrow_error.empty());
        const auto worker_tid = carrier->call(tid_function, zb::GuestCall{});
        const auto own_tls = carrier->call(tls_function, zb::GuestCall{});
        CHECK(worker_tid && static_cast<std::int32_t>(worker_tid->r0) == carrier->guest_tid());
        CHECK(own_tls && own_tls->r0 == carrier->guest_tls());
        worker_tids[index] = worker_tid->r0;
        worker_tls[index] = own_tls->r0;
        rendezvous.arrive_and_wait();
        const auto ticket = carrier->call(overlap, zb::GuestCall{});
        CHECK(ticket && ticket->r0 != 0xFFFFFFFFu);
        tickets[index] = ticket->r0;
        const auto contended = carrier->call(contention, args1(kIterations));
        CHECK(contended && contended->r0 == 0);
        const auto tid_after = carrier->call(tid_function, zb::GuestCall{});
        CHECK(tid_after && tid_after->r0 == worker_tids[index]);
    };
    std::thread first_worker(worker, 0);
    std::thread second_worker(worker, 1);
    first_worker.join();
    second_worker.join();
    CHECK(worker_tls[0] != 0 && worker_tls[1] != 0 && worker_tls[0] != worker_tls[1]);
    CHECK(worker_tids[0] != worker_tids[1]);
    CHECK(tickets[0] != tickets[1]);
    CHECK(read32(runtime, symbol("zb_contention_total")) == 2 * kIterations);
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    // The carrier path stays reusable.
    auto later = runtime.borrow(error);
    CHECK(later);
    const auto later_result = later->call(return_i, zb::GuestCall{});
    CHECK(later_result && later_result->r0 == 42);
    later.reset();
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    std::puts("library_runtime_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
```

```sh
ninja -C build/host host_call_dispatch_test library_runtime_test
```

Expected: compilation fails because `Process::create_borrower`,
`Process::destroy_borrower` and `LibraryRuntime::borrow` do not exist.

- [ ] **Step 2: Add borrowed JITs to Process**

In `core/include/zb/guest_thread.h`, replace:

```cpp
    // Number of host-to-guest calls active on this thread (call() frames).
    int call_depth = 0;
```

with:

```cpp
    // Number of host-to-guest calls active on this thread (call() frames).
    int call_depth = 0;
    // Code cache size for threads this thread clones; 0 selects kDefaultCodeCacheSize.
    std::size_t child_code_cache_size = 0;
```

In `core/include/zb/process.h`, replace:

```cpp
    std::size_t thread_count() const;
    GuestThread* find_thread(std::int32_t tid);
```

with:

```cpp
    // Real guest threads; borrowers are not counted.
    std::size_t thread_count() const;
    // Borrowers first, so tkill/tgkill aimed at a borrowed carrier's tid reach the borrower.
    GuestThread* find_thread(std::int32_t tid);

    // A JIT for the calling host thread that runs as `carrier`, a guest thread parked inside a
    // host call: its TLS, guest tid, a stack below its sp, its signal mask, alternate signal
    // stack and FPSCR. The carrier must stay parked until destroy_borrower. Costs one processor
    // id and a 32 MiB JIT; translated code starts cold. nullptr if no processor id is free.
    std::unique_ptr<GuestThread> create_borrower(GuestThread& carrier);
    // Copies the signal mask and alternate stack back to the still-parked carrier, moves signals
    // still pending on the borrower to it, and frees the borrower. Call on the borrowing thread.
    void destroy_borrower(std::unique_ptr<GuestThread> borrower, GuestThread& carrier);
```

In `core/include/zb/process.h`, replace:

```cpp
    std::vector<GuestThread*> threads_;
```

with:

```cpp
    std::vector<GuestThread*> threads_;
    std::vector<GuestThread*> borrowers_;
```

In `core/src/process.cpp`, replace:

```cpp
    for (GuestThread* t : threads_) t->invalidate(addr, len);
}
```

with:

```cpp
    for (GuestThread* t : threads_) t->invalidate(addr, len);
    for (GuestThread* t : borrowers_) t->invalidate(addr, len);
}
```

In `core/src/process.cpp`, replace:

```cpp
GuestThread* Process::find_thread(std::int32_t tid) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
```

with:

```cpp
GuestThread* Process::find_thread(std::int32_t tid) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : borrowers_) {
        if (t->tid == tid) return t;
    }
```

In `core/src/process.cpp`, replace:

```cpp
void Process::register_thread(GuestThread* thread) {
```

with:

```cpp
std::unique_ptr<GuestThread> Process::create_borrower(GuestThread& carrier) {
    const int processor_id = allocate_processor_id();
    if (processor_id < 0) return nullptr;
    auto borrower =
        std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id), precise_faults_);
    borrower->regs().fill(0);
    borrower->regs()[13] = carrier.regs()[13] & ~7u;
    borrower->ext_regs().fill(0);
    borrower->set_cpsr(kCpsrUserMode);
    borrower->set_fpscr(carrier.fpscr());
    borrower->set_tls(carrier.tls());
    borrower->tid = carrier.tid;
    borrower->sigmask = carrier.sigmask;
    borrower->altstack = carrier.altstack;
    std::lock_guard<std::mutex> lock(threads_mutex_);
    borrowers_.push_back(borrower.get());
    return borrower;
}

void Process::destroy_borrower(std::unique_ptr<GuestThread> borrower, GuestThread& carrier) {
    if (!borrower) return;
    carrier.sigmask = borrower->sigmask;
    carrier.altstack = borrower->altstack;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        std::erase(borrowers_, borrower.get());
    }
    g::siginfo32 info;
    while (borrower->take_signal(0, info)) carrier.post_signal(info);
    const std::size_t processor_id = borrower->processor_id();
    monitor_->ClearProcessor(processor_id);
    borrower.reset();
    std::lock_guard<std::mutex> lock(threads_mutex_);
    processor_ids_.reset(processor_id);
}

void Process::register_thread(GuestThread* thread) {
```

In `core/src/process.cpp`, replace:

```cpp
    auto child = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id), precise_faults_);
```

with:

```cpp
    const std::size_t code_cache_size =
        parent.child_code_cache_size != 0 ? parent.child_code_cache_size : kDefaultCodeCacheSize;
    auto child = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id),
                                               precise_faults_, code_cache_size);
```

```sh
ninja -C build/host host_call_dispatch_test; ctest --test-dir build/host -R '^host_call_dispatch_test$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 3: Implement carriers in the runtime**

`Carrier` is declared after `LibraryRuntime`; `ParkedCarrier` is defined before `Impl`.

Write `core/include/zb/library_runtime.h` with exactly this content:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_protocol.h"
#include "zb/native_call.h"
#include "zb/process.h"

namespace zb {

struct LibraryRuntimeOptions {
    std::string zbhost;   // host path of the arm32 zbhost executable
    std::string sysroot;  // host directory with the arm32 system files
    std::uint32_t target_sdk = 0;
    std::vector<std::string> envp;  // guest environment, e.g. LD_LIBRARY_PATH=...
    std::string preload;            // guest path dlopen'ed RTLD_GLOBAL before READY; empty: none
    std::chrono::milliseconds ready_timeout{10000};
};

// A guest process in library mode: zbhost runs on a service guest thread that owns the boot
// JIT and serves requests from inside its READY host call.
//
// The runtime is process-lifetime. Guest threads cannot be torn down, so after a successful
// start() the destructor logs and aborts; tests end with std::_Exit.
class LibraryRuntime {
public:
    class Carrier;

    LibraryRuntime();
    ~LibraryRuntime();
    LibraryRuntime(const LibraryRuntime&) = delete;
    LibraryRuntime& operator=(const LibraryRuntime&) = delete;

    // Receives every host call outside the runtime range 0xFE00-0xFEFF. Call before start().
    void set_host_call_handler(Process::HostCallHandler handler);
    bool start(const LibraryRuntimeOptions& options, std::string& error);

    // Guest dlopen/dlsym/dlerror on the service thread. Flags are ZB_GUEST_RTLD_* values.
    // Return 0 and set error (guest dlerror text) on failure.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::optional<GuestResult> call_on_service(std::uint32_t function, const GuestCall& args);
    // Nested call on the guest thread the calling host thread already runs (for host-call
    // handlers). nullopt if the calling host thread runs no guest code.
    std::optional<GuestResult> call_on_current(std::uint32_t function, const GuestCall& args);

    // Leases a new carrier (a guest pthread parked in PARK) to the calling host thread.
    // Spawning is serialized through the service thread. Fails on a host thread that already
    // runs guest code (the service thread, a borrower, a guest pthread): use call_on_current.
    std::unique_ptr<Carrier> borrow(std::string& error);

    GuestMemory& memory();
    // Valid after a successful start().
    const zb_service_api& service_api() const;
    // Real guest pthreads (service, carriers, guest-created threads); borrowers are not counted.
    std::size_t guest_thread_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A lease of one carrier, bound to the host thread that borrowed it. Each lease costs two
// processor ids and two JITs (a 2 MiB carrier JIT and a 32 MiB borrower JIT), and the borrower
// translates guest code cold. Destroying the lease (on the same host thread) releases the
// carrier, which returns from PARK and exits through bionic.
class LibraryRuntime::Carrier {
public:
    ~Carrier();
    Carrier(const Carrier&) = delete;
    Carrier& operator=(const Carrier&) = delete;

    std::optional<GuestResult> call(std::uint32_t function, const GuestCall& args);
    // Guest dlopen/dlsym on this carrier, with dlerror read on the same guest thread.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::int32_t guest_tid() const;
    std::uint32_t guest_tls() const;

private:
    friend class LibraryRuntime;
    struct State;
    explicit Carrier(std::unique_ptr<State> state);
    // Guest malloc'ed string buffer of this carrier, allocated on first use.
    bool ensure_scratch(std::string& error);

    std::unique_ptr<State> state_;
};

}  // namespace zb
```

Write `core/src/library_runtime.cpp` with exactly this content:

```cpp
#include "zb/library_runtime.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "zb/log.h"

namespace zb {

namespace {

constexpr std::chrono::milliseconds kCarrierParkTimeout{10000};

struct Response {
    bool ok = false;
    std::optional<GuestResult> result;
    std::string error;
};

struct Command {
    enum class Kind { Load, Symbol, Call, SpawnCarrier };
    Kind kind = Kind::Call;
    std::uint32_t value = 0;
    std::uint32_t flags = 0;
    std::string text;
    GuestCall args;
    std::promise<Response> done;
};

using Invoke = std::function<std::optional<GuestResult>(std::uint32_t function, const GuestCall& args)>;

std::string exit_message(int status) {
    if (status == ZB_HOST_EXIT_PRELOAD) return "zbhost could not preload a library (status 4)";
    return "zbhost exited with status " + std::to_string(status);
}

// A carrier guest pthread inside its PARK host call. Guarded by Impl::mutex. Only a carrier
// that is inside PARK is listed as available, so a lease never starts while the carrier runs
// guest code; a leased carrier does not leave PARK until it is released.
struct ParkedCarrier {
    enum class State { Available, Leased, Released };
    GuestThread* thread = nullptr;
    State state = State::Available;
    bool listed = false;
};

}  // namespace

struct LibraryRuntime::Impl {
    Process process;
    Process::HostCallHandler chained;
    std::thread runner;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable carrier_cv;
    std::deque<std::shared_ptr<Command>> commands;
    std::deque<std::shared_ptr<ParkedCarrier>> available;
    std::unordered_map<GuestThread*, std::shared_ptr<ParkedCarrier>> carriers;
    zb_service_api api{};
    GuestThread* service = nullptr;
    std::thread::id service_id;
    bool started = false;
    bool ready = false;
    bool finished = false;
    std::string startup_error;

    bool validate_api(std::uint32_t address, std::string& error) {
        const std::uint8_t* source = process.memory().host_ptr(address, sizeof api, kPageRead);
        if (source == nullptr) {
            error = "zbhost passed an unreadable service API";
            return false;
        }
        zb_service_api candidate;
        std::memcpy(&candidate, source, sizeof candidate);
        if (candidate.size != sizeof candidate || candidate.version != ZB_SERVICE_PROTOCOL_VERSION) {
            error = "zbhost service protocol mismatch";
            return false;
        }
        if (candidate.dlopen_fn == 0 || candidate.dlsym_fn == 0 || candidate.dlerror_fn == 0 ||
            candidate.spawn_carrier_fn == 0 || candidate.malloc_fn == 0 || candidate.free_fn == 0 ||
            candidate.scratch_size == 0 || candidate.scratch_size > ZB_SERVICE_SCRATCH_SIZE ||
            process.memory().host_ptr(candidate.scratch, candidate.scratch_size, kPageWrite) == nullptr) {
            error = "zbhost passed an invalid service API";
            return false;
        }
        api = candidate;
        return true;
    }

    bool copy_text(std::uint32_t buffer, std::uint32_t size, const std::string& text, std::string& error) {
        if (text.find('\0') != std::string::npos || text.size() + 1 > size) {
            error = "guest loader string does not fit its scratch buffer";
            return false;
        }
        std::uint8_t* destination = process.memory().host_ptr(buffer, text.size() + 1, kPageWrite);
        if (destination == nullptr) {
            error = "guest loader scratch buffer is not writable";
            return false;
        }
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    }

    std::string read_text(std::uint32_t address) {
        if (address == 0) return "guest dlerror returned null";
        std::string text;
        for (std::uint32_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
            const std::uint8_t* byte = process.memory().host_ptr(address + i, 1, kPageRead);
            if (byte == nullptr) return "guest dlerror returned an unreadable string";
            if (*byte == 0) return text;
            text.push_back(static_cast<char>(*byte));
        }
        return "guest dlerror string is not terminated";
    }

    // dlerror is per guest thread: read it through the same invoke as the failed call.
    std::string last_dlerror(const Invoke& invoke) {
        const auto result = invoke(api.dlerror_fn, GuestCall{});
        return result ? read_text(result->r0) : "guest dlerror call failed";
    }

    std::uint32_t load(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, const std::string& path,
                       std::uint32_t guest_flags, std::string& error) {
        if (!copy_text(buffer, size, path, error)) return 0;
        GuestCall args;
        args.regs = {buffer, guest_flags, 0, 0};
        const auto result = invoke(api.dlopen_fn, args);
        if (!result) {
            error = "guest dlopen call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    std::uint32_t symbol(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, std::uint32_t handle,
                         const std::string& name, std::string& error) {
        if (!copy_text(buffer, size, name, error)) return 0;
        GuestCall args;
        args.regs = {handle, buffer, 0, 0};
        const auto result = invoke(api.dlsym_fn, args);
        if (!result) {
            error = "guest dlsym call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    Response execute(GuestThread& thread, const Command& command) {
        const Invoke invoke = [&](std::uint32_t function, const GuestCall& args) {
            return process.call_guest(thread, function, args);
        };
        Response response;
        switch (command.kind) {
        case Command::Kind::Load:
            response.result = GuestResult{
                load(invoke, api.scratch, api.scratch_size, command.text, command.flags, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Symbol:
            response.result = GuestResult{
                symbol(invoke, api.scratch, api.scratch_size, command.value, command.text, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Call:
            response.result = invoke(command.value, command.args);
            response.ok = response.result.has_value();
            if (!response.ok) response.error = "guest service call failed";
            break;
        case Command::Kind::SpawnCarrier:
            // The carrier JIT only runs bionic thread start-up, parking and exit.
            thread.child_code_cache_size = kCarrierCodeCacheSize;
            response.result = invoke(api.spawn_carrier_fn, GuestCall{});
            thread.child_code_cache_size = 0;
            if (!response.result) {
                response.error = "guest pthread_create call failed";
            } else if (response.result->r0 != 0) {
                response.error = "guest pthread_create returned " + std::to_string(response.result->r0);
            } else {
                response.ok = true;
            }
            break;
        }
        return response;
    }

    // Serves commands until a signal must be delivered (r0 = AGAIN) or the guest is exiting.
    void serve(GuestThread& thread) {
        for (;;) {
            const std::uint32_t token = thread.park_token();
            if (thread.has_pending_signals(thread.sigmask)) {
                thread.regs()[0] = ZB_SERVICE_AGAIN;
                return;
            }
            std::shared_ptr<Command> command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    command = commands.front();
                    commands.pop_front();
                }
            }
            if (!command) {
                thread.park(token);
                continue;
            }
            command->done.set_value(execute(thread, *command));
            if (process.exiting()) {
                thread.regs()[0] = 1;
                return;
            }
        }
    }

    bool handle_ready(GuestThread& thread) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!ready) {
                std::string error;
                if (!validate_api(thread.regs()[0], error)) {
                    startup_error = std::move(error);
                    lock.unlock();
                    cv.notify_all();
                    thread.regs()[0] = 1;
                    return true;
                }
                service = &thread;
                service_id = std::this_thread::get_id();
                ready = true;
                lock.unlock();
                cv.notify_all();
            }
        }
        serve(thread);
        return true;
    }

    // PARK: publish this carrier, deliver its signals while it is unleased, and return 0 once a
    // borrower has released it.
    bool park_carrier(GuestThread& thread) {
        std::shared_ptr<ParkedCarrier> record;
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::shared_ptr<ParkedCarrier>& slot = carriers[&thread];
            if (!slot) {
                slot = std::make_shared<ParkedCarrier>();
                slot->thread = &thread;
            }
            record = slot;
        }
        bool detached = false;
        for (;;) {
            const std::uint32_t token = thread.park_token();
            bool published = false;
            bool leased = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (record->state == ParkedCarrier::State::Released) {
                    carriers.erase(&thread);
                    break;
                }
                if (record->state == ParkedCarrier::State::Available) {
                    if (thread.has_pending_signals(thread.sigmask)) {
                        if (record->listed) {
                            std::erase(available, record);
                            record->listed = false;
                        }
                        thread.regs()[0] = ZB_SERVICE_AGAIN;
                        return true;
                    }
                    if (!record->listed) {
                        available.push_back(record);
                        record->listed = true;
                        published = true;
                    }
                } else {
                    leased = true;
                }
            }
            if (published) carrier_cv.notify_all();
            if (leased && !detached) {
                // Host signals landing on this host thread go to the process signal target
                // while the carrier's identity runs on the borrower.
                Process::set_current_thread(nullptr);
                detached = true;
            }
            thread.park(token);
        }
        if (detached) Process::set_current_thread(&thread);
        thread.regs()[0] = 0;
        return true;
    }

    bool handle_host_call(std::uint32_t index, GuestThread& thread) {
        if (index >= ZB_RUNTIME_HOST_CALL_FIRST && index <= ZB_RUNTIME_HOST_CALL_LAST) {
            if (index == ZB_SERVICE_READY_INDEX) return handle_ready(thread);
            if (index == ZB_CARRIER_PARK_INDEX) return park_carrier(thread);
            return false;
        }
        return chained && chained(index, thread);
    }

    Response submit(std::shared_ptr<Command> command) {
        std::future<Response> future = command->done.get_future();
        GuestThread* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || finished) return {false, std::nullopt, "guest library runtime is not running"};
            if (std::this_thread::get_id() == service_id) {
                return {false, std::nullopt, "guest service request made on the service thread; use call_on_current"};
            }
            commands.push_back(std::move(command));
            target = service;
        }
        target->wake();
        return future.get();
    }
};

struct LibraryRuntime::Carrier::State {
    Impl* impl = nullptr;
    std::shared_ptr<ParkedCarrier> parked;
    std::unique_ptr<GuestThread> borrower;
    std::thread::id owner;
    std::uint32_t scratch = 0;
};

LibraryRuntime::LibraryRuntime() : impl_(std::make_unique<Impl>()) {}

LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready || !impl_->finished) {
            log("LibraryRuntime destroyed after start; the runtime is process-lifetime");
            std::abort();
        }
    }
    impl_->runner.join();
}

void LibraryRuntime::set_host_call_handler(Process::HostCallHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) {
        log("LibraryRuntime::set_host_call_handler called after start");
        std::abort();
    }
    impl_->chained = std::move(handler);
}

bool LibraryRuntime::start(const LibraryRuntimeOptions& options, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) {
            error = "guest library runtime was already started";
            return false;
        }
        impl_->started = true;
    }
    Impl* impl = impl_.get();
    impl->process.set_sysroot(options.sysroot);
    impl->process.set_host_call_handler(
        [impl](std::uint32_t index, GuestThread& thread) { return impl->handle_host_call(index, thread); });
    std::vector<std::string> argv = {options.zbhost, std::to_string(options.target_sdk)};
    if (!options.preload.empty()) argv.push_back(options.preload);
    impl->runner = std::thread([impl, argv, options] {
        const int status = impl->process.run(options.zbhost, argv, options.envp);
        std::deque<std::shared_ptr<Command>> pending;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->finished = true;
            if (!impl->ready && impl->startup_error.empty()) impl->startup_error = exit_message(status);
            pending.swap(impl->commands);
        }
        impl->cv.notify_all();
        impl->carrier_cv.notify_all();
        for (const auto& command : pending) command->done.set_value({false, std::nullopt, exit_message(status)});
    });

    std::unique_lock<std::mutex> lock(impl->mutex);
    const bool settled = impl->cv.wait_for(lock, options.ready_timeout, [impl] {
        return impl->ready || impl->finished || !impl->startup_error.empty();
    });
    if (impl->ready) return true;
    error = settled ? impl->startup_error
                    : "zbhost did not report ready within " + std::to_string(options.ready_timeout.count()) + " ms";
    return false;
}

std::uint32_t LibraryRuntime::load_library(const std::string& path, std::uint32_t guest_flags, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Load;
    command->text = path;
    command->flags = guest_flags;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::uint32_t LibraryRuntime::find_symbol(std::uint32_t handle, const std::string& name, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Symbol;
    command->value = handle;
    command->text = name;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::optional<GuestResult> LibraryRuntime::call_on_service(std::uint32_t function, const GuestCall& args) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Call;
    command->value = function;
    command->args = args;
    Response response = impl_->submit(std::move(command));
    return response.ok ? response.result : std::nullopt;
}

std::optional<GuestResult> LibraryRuntime::call_on_current(std::uint32_t function, const GuestCall& args) {
    GuestThread* thread = Process::current_thread();
    if (thread == nullptr) return std::nullopt;
    return impl_->process.call_guest(*thread, function, args);
}

std::unique_ptr<LibraryRuntime::Carrier> LibraryRuntime::borrow(std::string& error) {
    if (Process::current_thread() != nullptr) {
        error = "the calling host thread already runs guest code; use call_on_current";
        return nullptr;
    }
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::SpawnCarrier;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return nullptr;
    }

    std::shared_ptr<ParkedCarrier> record;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->carrier_cv.wait_for(lock, kCarrierParkTimeout,
                                   [&] { return !impl_->available.empty() || impl_->finished; });
        if (impl_->available.empty()) {
            error = impl_->finished ? "zbhost exited" : "guest carrier did not park within 10000 ms";
            return nullptr;
        }
        record = impl_->available.front();
        impl_->available.pop_front();
        record->listed = false;
        record->state = ParkedCarrier::State::Leased;
        record->thread->wake();
    }

    std::unique_ptr<GuestThread> borrower = impl_->process.create_borrower(*record->thread);
    if (!borrower) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        record->state = ParkedCarrier::State::Released;
        record->thread->wake();
        error = "no Dynarmic processor id is available for a borrower";
        return nullptr;
    }
    auto state = std::make_unique<Carrier::State>();
    state->impl = impl_.get();
    state->parked = std::move(record);
    state->borrower = std::move(borrower);
    state->owner = std::this_thread::get_id();
    return std::unique_ptr<Carrier>(new Carrier(std::move(state)));
}

GuestMemory& LibraryRuntime::memory() {
    return impl_->process.memory();
}

const zb_service_api& LibraryRuntime::service_api() const {
    return impl_->api;
}

std::size_t LibraryRuntime::guest_thread_count() const {
    return impl_->process.thread_count();
}

LibraryRuntime::Carrier::Carrier(std::unique_ptr<State> state) : state_(std::move(state)) {}

LibraryRuntime::Carrier::~Carrier() {
    if (state_->owner != std::this_thread::get_id()) {
        log("carrier lease released on a different host thread");
        std::abort();
    }
    Impl* impl = state_->impl;
    if (state_->scratch != 0) {
        GuestCall args;
        args.regs = {state_->scratch, 0, 0, 0};
        (void)call(impl->api.free_fn, args);
    }
    GuestThread& carrier = *state_->parked->thread;
    impl->process.destroy_borrower(std::move(state_->borrower), carrier);
    // Wake under the lock: once the carrier sees Released it may exit and free its GuestThread.
    std::lock_guard<std::mutex> lock(impl->mutex);
    state_->parked->state = ParkedCarrier::State::Released;
    carrier.wake();
}

std::optional<GuestResult> LibraryRuntime::Carrier::call(std::uint32_t function, const GuestCall& args) {
    if (state_->owner != std::this_thread::get_id()) {
        log("carrier lease used on a different host thread");
        std::abort();
    }
    GuestThread* borrower = state_->borrower.get();
    GuestThread* previous = Process::current_thread();
    if (previous != nullptr && previous != borrower) {
        log("carrier lease used while the host thread runs other guest code");
        return std::nullopt;
    }
    Process::set_current_thread(borrower);
    auto result = state_->impl->process.call_guest(*borrower, function, args);
    Process::set_current_thread(previous);
    return result;
}

bool LibraryRuntime::Carrier::ensure_scratch(std::string& error) {
    if (state_->scratch != 0) return true;
    GuestCall args;
    args.regs = {ZB_SERVICE_SCRATCH_SIZE, 0, 0, 0};
    const auto result = call(state_->impl->api.malloc_fn, args);
    if (!result || result->r0 == 0) {
        error = "guest malloc for the carrier scratch buffer failed";
        return false;
    }
    state_->scratch = result->r0;
    return true;
}

std::uint32_t LibraryRuntime::Carrier::load_library(const std::string& path, std::uint32_t guest_flags,
                                                    std::string& error) {
    if (!ensure_scratch(error)) return 0;
    const Invoke invoke = [this](std::uint32_t function, const GuestCall& args) { return call(function, args); };
    return state_->impl->load(invoke, state_->scratch, ZB_SERVICE_SCRATCH_SIZE, path, guest_flags, error);
}

std::uint32_t LibraryRuntime::Carrier::find_symbol(std::uint32_t handle, const std::string& name,
                                                   std::string& error) {
    if (!ensure_scratch(error)) return 0;
    const Invoke invoke = [this](std::uint32_t function, const GuestCall& args) { return call(function, args); };
    return state_->impl->symbol(invoke, state_->scratch, ZB_SERVICE_SCRATCH_SIZE, handle, name, error);
}

std::int32_t LibraryRuntime::Carrier::guest_tid() const {
    return state_->borrower->tid;
}

std::uint32_t LibraryRuntime::Carrier::guest_tls() const {
    return state_->borrower->tls();
}

}  // namespace zb
```

```sh
ninja -C build/host library_runtime_test; ctest --test-dir build/host -R '^library_runtime_test$' --output-on-failure -V | grep -E 'latency|PASS|CHECK'
```

Expected (timings vary; recorded on this machine while prototyping):

```text
carrier latency: borrow 30.92 ms, first call 0.43 ms, warm call 0.001 ms
library_runtime_test PASS
```

- [ ] **Step 4: Show that routing and inheritance are load-bearing**

1. Temporarily remove the `borrowers_` loop from `Process::find_thread`. Expected:
   `CHECK failed: read32(runtime, symbol("zb_usr1_count")) == 1`.
2. Restore it and temporarily remove `borrower->sigmask = carrier.sigmask;` from
   `create_borrower`. Expected: `CHECK failed: blocked && blocked->r0 == 1` (and
   `host_call_dispatch_test` fails).
3. Restore it.

```sh
ninja -C build/host library_runtime_test host_call_dispatch_test; build/host/tests/host/library_runtime_test sysroot build/guest/zbhost build/guest/lib/libzbcallprobe.so 2>&1 | grep -E 'CHECK|PASS'
```

- [ ] **Step 5: Verify concurrency and commit**

```sh
fails=0; for i in $(seq 50); do build/host/tests/host/library_runtime_test sysroot build/guest/zbhost build/guest/lib/libzbcallprobe.so >/dev/null 2>&1 || fails=$((fails+1)); done; echo "library_runtime_test failures: $fails/50"
```

```sh
tools/build_guest.sh; ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/run_guest_tests.sh; git status --short
```

Expected: `failures: 0/50`, 14/14 host tests, guest cases pass. If the configured
Dynarmic build supports ThreadSanitizer, also run `library_runtime_test` under that build;
record an unsupported sanitizer build in the task ledger rather than silently skipping it.

```sh
git add core/include/zb/guest_thread.h core/include/zb/process.h core/src/process.cpp core/include/zb/library_runtime.h core/src/library_runtime.cpp tests/host/host_call_dispatch_test.cpp tests/host/library_runtime_test.cpp; git commit -m "core: run guest calls on borrowed carrier threads"
```

---

## Task 6: Regression, Android link, and status docs

**Status:** ready, plan revised after review (D14).

**Files:**
- Modify: `CLAUDE.md`
- Modify: `AGENTS.md`

**Interfaces:** no code interface changes; this task verifies and records the Phase 4b
contract.

- [ ] **Step 1: Run all local checks**

```sh
tools/build_guest.sh; ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/run_guest_tests.sh
```

```sh
N=$HOME/android-ndk-r29; mkdir -p build/boost-headers; ln -sfn /usr/include/boost build/boost-headers/boost; cmake -S . -B build/android-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$N/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DZB_BUILD_TESTS=OFF -DBoost_INCLUDE_DIR=$PWD/build/boost-headers; ninja -C build/android-arm64 zbridge zbrun
```

Expected: 14/14 host tests, all 9 guest cases pass or skip only `or_dlopen_dynamic`
without the APK, and both Android arm64 targets link.

- [ ] **Step 2: Review invariants before closing 4b**

- Every call path goes through `dispatch_stop` and `after_stop`; no duplicated syscall loop.
- `kHostReturnSwi` ends only an active call whose `sp` matches; any other one is SIGILL.
- A thread exit inside a call ends the host process.
- A service JIT is touched only by its boot thread; service requests from that thread fail.
- A borrower JIT is created, called, and destroyed on the borrowing host thread.
- A carrier is leased only while inside `PARK` and does not leave `PARK` until released.
- Signals posted to a parked service thread or unleased carrier wake it and are delivered
  through `ZB_SERVICE_AGAIN`.
- A leased carrier cannot resume before its borrower is removed from tid routing and its
  sigmask and altstack are copied back.
- The runtime is never destroyed after start; tests end with `_Exit`.
- Generated host-call indices stay below `0xFE00`.
- `third_party/dynarmic` is not staged.

- [ ] **Step 3: Update handoff docs and commit**

In `CLAUDE.md` "Current state", replace "Next: plan 4b ..." with Phase 4b done
(library-mode runtime, carriers, plan path) and "Next: plan 4c (generated guest `JNIEnv`,
host JNI backend, mock JNI test)". Add to "Known gotchas": guest RTLD values differ from
host `<dlfcn.h>`; the library runtime is process-lifetime; threads parked in host calls
must use the futex park so signals reach them. In `AGENTS.md`, mark Tasks 4-6 done with
their commits and set 4c as next.

```sh
git add CLAUDE.md AGENTS.md; git commit -m "docs: mark Phase 4b complete"
```

---

## Acceptance

Phase 4b is complete when all of these pass on this machine, together with the complete
pre-4b host and guest suites and the Android arm64 link:

`guest_call_test`:
- nested call from inside a host-call stop handler, with an observable register mutation;
- Thumb target;
- stopped CPSR with IT bits and the E bit set (D2);
- a signal posted during a call, delivered through the guest handler and `rt_sigreturn`,
  after which the call returns correctly;
- a return with a mismatched `sp` (D4), and a return svc outside any call;
- `exit` inside a call as a death test in a forked child, ending the host process with
  status 1 and the D3 log line;
- a fatal signal inside a call on the only guest thread returning the signal status.

`host_call_dispatch_test`:
- host-call dispatch through `set_host_call_handler`;
- borrower inheritance (TLS, tid, sp, sigmask, altstack, FPSCR), borrower-first
  `find_thread`, and copy-back of sigmask, altstack and pending signals (D7).

`zbhost_protocol_test`:
- protocol version 2 API fields; a failed preload exits with status 4 (D11).

`library_runtime_test`:
- guest RTLD constants, `NOLOAD`, `RTLD_DEFAULT` (D1);
- dlopen/dlsym/dlerror on the service thread and on a carrier (D9);
- `store_native_result` for every return type through the guest probe, on both paths;
- the mixed softfp signature through `marshal_native_args`;
- host-call chaining with nested `call_on_current` on the service thread and a borrower (D8);
- misuse guards: service request on the service thread, `borrow` from a thread running
  guest code, `call_on_current` without guest code (D10);
- `start` reporting a failed preload (D11);
- SIGALRM delivered to the parked service thread, armed on the service and on a borrower (D6);
- tgkill to the carrier tid delivered on the borrower;
- sigmask and altstack inheritance through a real carrier (D7);
- two concurrent borrowers rendezvousing in guest code under `pthread_mutex`,
  `malloc`/`free`, `errno` and `pthread_self` contention with consistent tid/TLS;
- lease release letting `guest_thread_count()` settle, and a later borrow;
- the first-borrow and first-call latency report (D12);
- process-lifetime exit with `_Exit(0)` (D5).

## Following plan boundaries

- **4c** consumes `LibraryRuntime::Carrier`, `call_on_current`, `set_host_call_handler`,
  `memory`, handle tables, shorties, and `GuestCall`. It generates `libzbjni.so` (passed as
  `LibraryRuntimeOptions::preload`), implements the flat host JNI backend, and runs the
  mock JNI suite without ART.
- **4d** owns ART and launcher integration: proxy libraries, `onProxyLoaded`, loading and
  guest `JNI_OnLoad` on a carrier, export binding, per-method `RegisterNatives`, carrier
  caching in a host pthread-key destructor, T7, and the Orange Roulette Phase 4 smoke test.
