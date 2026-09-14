# Phase 4b: Library Runtime, Host-to-Guest Calls, and Carriers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Phase 1-3 executable runner into a reusable library-mode guest
process that loads an arm32 shared library, resolves a symbol, and calls it on the
service guest thread or a carrier borrowed by the current host thread.

**Architecture:** The guest `zbhost` enters one reserved host call and becomes a
command service owned by its boot JIT. Foreign host threads borrow the TLS, stack, and
guest tid of parked real guest pthreads while running their own JIT. Nested guest calls
reuse the existing syscall, host-call, fault, and signal dispatcher.

**Tech Stack:** C++20, C11, Dynarmic A32, arm32 bionic/NDK r29, pthreads, CMake/Ninja,
host tests and guest integration tests.

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`, building on
`docs/superpowers/specs/2026-09-13-guest-system-boundary-design.md`.

## Global constraints

- Guest Java stays on the real 64-bit ART; only guest native code is translated.
- Guest libraries use the real arm32 bionic linker; host code never loads them.
- Host work never runs inside a Dynarmic callback.
- JNI native entry is AAPCS32 base/softfp, including hard-float-built guest libraries.
- Guest pointers are offsets inside the 4 GiB reservation; host pointers never cross.
- A host thread borrowing a carrier keeps that carrier's TLS, stack, and guest tid.
- `svc #0x5AFFFF` is reserved exclusively for an active host-to-guest return.
- Code and scripts are English ASCII only; commits are local English imperative commits.
- The existing dirty `third_party/dynarmic` worktree state is never staged.

---

## Scope

This plan implements only the execution substrate needed by JNI. It does not create a
guest `JNIEnv`, call ART, bind native methods, or change the launcher. Those belong to
plans 4c and 4d.

## Approved architecture carried forward

- Guest bionic and the guest linker continue to run translated; host code never calls
  arm32 `dlopen` or `dlsym` directly.
- Dynarmic callbacks only record a `Stop` and halt. Syscalls, host calls, signals, and
  service waits happen after `Run()` returns.
- A host-to-guest call saves the stopped guest CPU state, lays out `GuestCall`, sets
  `lr` to a fixed return `svc`, runs through the normal stop dispatcher, captures
  `r0:r1`, and restores the stopped CPU state.
- The service thread owns the JIT that booted `zbhost`. Requests are serialized through
  its ready host call.
- A Java/UI/GL host thread never executes on the service JIT. It borrows a real guest
  pthread's TLS, stack, and guest tid, but uses a separate JIT on the entering host
  thread. The parked carrier JIT stays on its original host pthread.
- `tgkill`/`tkill` and emulated `gettid` see the carrier tid while it is borrowed.
- No run-time executable memory is introduced.

## File structure

| File | Responsibility |
|---|---|
| `core/include/zb/guest_thread.h`, `core/src/guest_thread.cpp` | nested CPU call frame and `r0:r1` result |
| `core/include/zb/process.h`, `core/src/process.cpp` | reusable stop dispatch, host-call hook, borrowed JIT registry |
| `core/include/zb/library_protocol.h` | fixed C-compatible `zbhost` handshake ABI |
| `guest/zbhost/zbhost.c` | guest service entry and carrier creation |
| `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp` | service queue, `dlopen`/`dlsym`, carrier leases |
| `guest/testlib/zbcallprobe.c` | softfp ABI probe shared library |
| `tests/host/guest_call_test.cpp` | nested call state and stack layout |
| `tests/host/host_call_dispatch_test.cpp` | reusable Process host-call dispatch |
| `tests/host/library_runtime_test.cpp` | real linker, library calls, two simultaneous borrowers |
| `tools/build_guest.sh`, `core/CMakeLists.txt`, `tests/host/CMakeLists.txt` | build wiring |

Commands are run from `/home/Zailox/ZettaBridge`. Every task ends with:

```sh
ninja -C build/host && ctest --test-dir build/host --output-on-failure
```

Commit locally after every task. Push only the dedicated Codex branch, and only after
a remote is configured.

---

## Task 1: Nested host-to-guest call frame

**Files:**
- Modify: `core/include/zb/guest_thread.h`
- Modify: `core/src/guest_thread.cpp`
- Create: `tests/host/guest_call_test.cpp`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:** `GuestThread::call`, `GuestResult`, `GuestStopHandler`,
`kHostReturnAddress`.

- [ ] **Step 1: Add the failing host test**

`tests/host/guest_call_test.cpp` maps a tiny ARM function and a writable stack. The
function adds `r0`, `r1`, and its first stack word, then returns through the fixed SVC.
The test also makes the stop handler service an intermediate host call and verifies
that all stopped CPU state is restored after the nested call.

```cpp
#include <sys/mman.h>

#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/native_call.h"

namespace {

void put32(zb::GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    std::memcpy(mem.base() + addr, &value, sizeof value);
}

}  // namespace

int main() {
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
    put32(mem, zb::kHostReturnAddress, 0xEF5AFFFF);  // svc #0x5affff
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    CHECK(mem.protect(0xFFFF0000, 0x1000, PROT_READ | PROT_EXEC));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(mem, &monitor, 0);
    thread.regs().fill(0xA5A5A5A5);
    thread.ext_regs().fill(0x5A5A5A5A);
    thread.regs()[13] = 0x22000;
    thread.regs()[15] = 0x12345678;
    thread.set_cpsr(0x10);
    thread.set_fpscr(0x01000000);
    const auto saved_regs = thread.regs();
    const auto saved_ext = thread.ext_regs();

    zb::GuestCall call;
    call.regs = {10, 20, 0, 0};
    call.stack = {12};
    int host_calls = 0;
    const auto result = thread.call(0x10000, call, [&](const zb::Stop& stop) {
        CHECK(stop.kind == zb::StopKind::Svc && stop.swi == 0x5A0010);
        ++host_calls;
        thread.regs()[0] += 0;  // handler may update call registers
        return true;
    });

    CHECK(result && result->r0 == 42 && result->r1 == 20);
    CHECK(host_calls == 1);
    CHECK(thread.regs() == saved_regs);
    CHECK(thread.ext_regs() == saved_ext);
    CHECK(thread.cpsr() == 0x10 && thread.fpscr() == 0x01000000);

    zb::GuestCall too_large;
    too_large.stack.resize(0x1000);
    thread.regs()[13] = 0x20004;
    CHECK(!thread.call(0x10000, too_large, [](const zb::Stop&) { return false; }));

    std::puts("guest_call_test PASS");
    return 0;
}
```

Add `guest_call_test` to `ZB_HOST_TESTS`. Run it and confirm that compilation fails
because `GuestThread::call` and `GuestResult` do not exist.

- [ ] **Step 2: Add the call contract and implementation**

In `guest_thread.h`, include `<functional>` and `zb/native_call.h`, then add:

```cpp
inline constexpr std::uint32_t kHostReturnAddress = 0xFFFF0F00;

struct GuestResult {
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
};

using GuestStopHandler = std::function<bool(const Stop&)>;
```

Add this public method to `GuestThread`:

```cpp
// Runs one nested guest function. The stopped CPU state is restored on every return path.
// The handler runs outside Dynarmic and returns true to resume or false to fail the call.
std::optional<GuestResult> call(std::uint32_t target, const GuestCall& args,
                                const GuestStopHandler& handle_stop);
```

Add this implementation to `guest_thread.cpp`. It deliberately restores CPU state but
not TLS, signal masks, or other emulated kernel state changed by the guest function.

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
    set_cpsr((saved_cpsr & ~0x20u) | ((target & 1u) ? 0x20u : 0));

    std::optional<GuestResult> result;
    for (;;) {
        const Stop stop = run();
        if (stop.kind == StopKind::Svc && stop.swi == kHostReturnSwi) {
            result = GuestResult{regs()[0], regs()[1]};
            break;
        }
        if (!handle_stop(stop)) break;
    }
    restore();
    return result;
}
```

The kuser mapping in `process.cpp` must place `0xEF5AFFFF` at offset `0xF00` before
the page becomes RX. Keep the existing helper offsets unchanged.

- [ ] **Step 3: Verify and commit**

```sh
ninja -C build/host guest_call_test
ctest --test-dir build/host -R '^guest_call_test$' --output-on-failure
ninja -C build/host
ctest --test-dir build/host --output-on-failure
git status --short
git add core/include/zb/guest_thread.h core/src/guest_thread.cpp \
    core/src/process.cpp tests/host/guest_call_test.cpp tests/host/CMakeLists.txt
git commit -m "core: add nested host-to-guest call frames"
```

---

## Task 2: Reusable Process stop dispatch

**Files:**
- Modify: `core/include/zb/process.h`
- Modify: `core/src/process.cpp`
- Modify: `core/src/syscalls.cpp`
- Create: `guest/tests/host_call_static.c`
- Create: `tests/host/host_call_dispatch_test.cpp`
- Modify: `tools/build_guest.sh`, `tests/host/CMakeLists.txt`

**Interfaces:** `Process::set_host_call_handler`, `Process::dispatch_stop`,
`Process::call_guest`, `host_call_name`.

- [ ] **Step 1: Add the failing dispatch test**

The static guest performs `svc #0x5afe10`, checks the value returned in `r0`, and exits.

```c
#include <stdint.h>

int main(void) {
    register uint32_t r0 __asm__("r0") = 7;
    __asm__ volatile("svc #0x5afe10" : "+r"(r0) : : "memory");
    return r0 == 49 ? 0 : 1;
}
```

`host_call_dispatch_test.cpp` creates a `Process`, installs a handler that recognizes
index `0xFE10`, checks `r0 == 7`, writes `49`, and runs the binary passed as argv[1].
It fails to compile until the following API exists:

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

The public `process.h` additions are:

```cpp
using HostCallHandler = std::function<bool(std::uint32_t index, GuestThread& thread)>;
void set_host_call_handler(HostCallHandler handler) { host_call_handler_ = std::move(handler); }
std::optional<GuestResult> call_guest(GuestThread& thread, std::uint32_t target,
                                      const GuestCall& args);
```

Include `<functional>` and add `bool dispatch_stop(GuestThread&, const Stop&);` plus
`HostCallHandler host_call_handler_;` in the private section.

Add `host_call_dispatch_test` as a separately declared test, like `elf_loader_test`,
with `${CMAKE_SOURCE_DIR}/build/guest/host_call_static` as its argument.

```cmake
add_executable(host_call_dispatch_test host_call_dispatch_test.cpp)
target_link_libraries(host_call_dispatch_test PRIVATE zbcore)
add_test(NAME host_call_dispatch_test COMMAND host_call_dispatch_test
    ${CMAKE_SOURCE_DIR}/build/guest/host_call_static)
```

- [ ] **Step 2: Extract one stop dispatcher**

Add private `bool dispatch_stop(GuestThread&, const Stop&)` to `Process`. Move the
existing switch body from `thread_loop` into it without changing syscall, fault,
signal, crash, or exit behavior. Its return value is true only when execution may
resume. The host-call arm becomes:

```cpp
if ((stop.swi & 0xFF0000u) == kHostCallBase && stop.swi != kHostReturnSwi) {
    const std::uint32_t index = stop.swi & 0xFFFFu;
    if (host_call_handler_ && host_call_handler_(index, thread)) return true;
    if (first_time(kSeenHostCall | index)) {
        const auto [library, name] = host_call_name(index);
        log("host call %s:%s is not implemented yet", library, name);
    }
    thread.regs()[0] = 0;
    return true;
}
```

`thread_loop` becomes a loop around `thread.run()`, `dispatch_stop`, and the existing
pending-signal check. `call_guest` uses the identical path:

```cpp
bool Process::dispatch_stop(GuestThread& thread, const Stop& stop) {
    switch (stop.kind) {
    case StopKind::Svc:
        if (stop.swi == 0) {
            if (handle_syscall(*this, thread)) return true;
            if (exiting_ && thread_count() > 1) exit_host_process();
            return false;
        }
        if ((stop.swi & 0xFF0000u) == kHostCallBase && stop.swi != kHostReturnSwi) {
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
        if (deliver_fault(thread, stop)) return true;
        crash_report(stop, thread);
        request_exit(128 + (stop.kind == StopKind::MemoryFault ? SIGSEGV : SIGILL));
        if (thread_count() > 1) exit_host_process();
        return false;
    case StopKind::None:
        log("guest stopped without a reason at pc 0x%08x", stop.pc);
        request_exit(1);
        if (thread_count() > 1) exit_host_process();
        return false;
    }
    return false;
}

void Process::thread_loop(GuestThread& thread) {
    set_current_thread(&thread);
    for (;;) {
        if (!dispatch_stop(thread, thread.run())) return;
        if (thread.has_pending_signals(thread.sigmask) && !dispatch_pending_signals(thread)) {
            if (thread_count() > 1) exit_host_process();
            return;
        }
    }
}

std::optional<GuestResult> Process::call_guest(GuestThread& thread, std::uint32_t target,
                                               const GuestCall& args) {
    return thread.call(target, args, [&](const Stop& stop) {
        if (!dispatch_stop(thread, stop)) return false;
        return !thread.has_pending_signals(thread.sigmask) || dispatch_pending_signals(thread);
    });
}
```

Add this lookup beside `kHostCallNames`:

```cpp
std::pair<const char*, const char*> host_call_name(std::uint32_t index) {
    for (const auto& host_call : kHostCallNames) {
        if (host_call.index == index) return {host_call.library, host_call.name};
    }
    return {"?", "?"};
}
```

Do not special-case syscalls made during nested calls. A guest `exit` or fatal signal
fails the call through the same path as the main loop.

- [ ] **Step 3: Make guest tid explicit**

Change `NR_gettid` and the return from `NR_set_tid_address` to use `thread.tid` when it
is nonzero. Normal guest pthreads already store their host tid, so this is behaviorally
unchanged until carriers arrive:

```cpp
const auto guest_tid = [&] {
    return thread.tid != 0 ? thread.tid : static_cast<std::int32_t>(::syscall(SYS_gettid));
};
```

- [ ] **Step 4: Verify and commit**

```sh
tools/build_guest.sh
ninja -C build/host host_call_dispatch_test
ctest --test-dir build/host -R '^host_call_dispatch_test$' --output-on-failure
ninja -C build/host
ctest --test-dir build/host --output-on-failure
git status --short
git add core/include/zb/process.h core/src/process.cpp core/src/syscalls.cpp \
    guest/tests/host_call_static.c tests/host/host_call_dispatch_test.cpp \
    tools/build_guest.sh tests/host/CMakeLists.txt
git commit -m "core: reuse stop dispatch for nested guest calls"
```

---

## Task 3: Fixed `zbhost` service protocol

**Files:**
- Create: `core/include/zb/library_protocol.h`
- Create: `guest/zbhost/zbhost.c`
- Create: `tests/host/zbhost_protocol_test.cpp`
- Modify: `tools/build_guest.sh`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:** `zb_service_api`, `ZB_SERVICE_READY_INDEX`,
`ZB_CARRIER_PARK_INDEX`, `ZB_SERVICE_PROTOCOL_VERSION`.

- [ ] **Step 1: Write the failing handshake test and define the C-compatible protocol**

Write this test first:

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

Declare it with the three paths it consumes:

```cmake
add_executable(zbhost_protocol_test zbhost_protocol_test.cpp)
target_link_libraries(zbhost_protocol_test PRIVATE zbcore)
add_test(NAME zbhost_protocol_test COMMAND zbhost_protocol_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)
```

Run `ninja -C build/host zbhost_protocol_test`. Expected: compilation fails because
`zb/library_protocol.h` does not exist. Only then add the protocol header and guest
source below.

`library_protocol.h` contains no C++ types:

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

The complete guest service is:

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

Build it as `build/guest/zbhost` by adding this command after the dynamic-C loop in
`tools/build_guest.sh`:

```sh
"$CC" -O2 -Wall -I"$ROOT/core/include" -o "$OUT/zbhost" \
    "$ROOT/guest/zbhost/zbhost.c" -ldl
```

Verify with `readelf` that it is ELF32 ARM, has `/system/bin/linker`, and has no
undefined ZettaBridge symbol.

- [ ] **Step 2: Verify and commit**

```sh
tools/build_guest.sh
ninja -C build/host zbhost_protocol_test
ctest --test-dir build/host -R '^zbhost_protocol_test$' --output-on-failure
ninja -C build/host
ctest --test-dir build/host --output-on-failure
tools/run_guest_tests.sh
git status --short
git add core/include/zb/library_protocol.h guest/zbhost/zbhost.c \
    tests/host/zbhost_protocol_test.cpp tools/build_guest.sh tests/host/CMakeLists.txt
git commit -m "guest: add library-mode service executable"
```

---

## Task 4: Service-thread library runtime

**Files:**
- Create: `core/include/zb/library_runtime.h`
- Create: `core/src/library_runtime.cpp`
- Modify: `core/CMakeLists.txt`
- Create: `guest/testlib/zbcallprobe.c`
- Create: `tests/host/library_runtime_test.cpp`
- Modify: `tools/build_guest.sh`, `tests/host/CMakeLists.txt`

**Interfaces:** `LibraryRuntime::start`, `LibraryRuntime::load_library`,
`LibraryRuntime::find_symbol`, `LibraryRuntime::call_on_service`.

- [ ] **Step 1: Add the softfp probe test fixture**

Build `zbcallprobe.c` as `libzbcallprobe.so`. Every exported function uses
`__attribute__((pcs("aapcs")))` so its public boundary is explicitly base AAPCS even
if a future toolchain default changes.

Add this command after `libzbthrow.so` is built:

```sh
"$CC" -shared -O2 -Wall -Wl,-soname,libzbcallprobe.so \
    -o "$OUT/lib/libzbcallprobe.so" "$ROOT/guest/testlib/zbcallprobe.c"
```

```c
#include <stdint.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default"), pcs("aapcs")))

API int32_t zb_probe_mix(uint32_t env, uint32_t self, uint8_t z, int8_t b,
                         uint16_t c, int16_t s, int32_t i, int64_t j,
                         float f, double d, uint32_t ref) {
    if (env != 0xe000 || self != 0x1077 || z != 1 || b != -2 || c != 0x1234 ||
        s != -3 || i != 4 || j != INT64_C(0x1122334455667788) ||
        f != 1.5f || d != -2.25 || ref != 0x1099) return -1;
    return 42;
}

API uint32_t zb_probe_tls(uint32_t env, uint32_t self) {
    uint32_t tls;
    (void)env;
    (void)self;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
    return tls;
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

static uint32_t overlap_count;

API int32_t zb_probe_overlap(uint32_t env, uint32_t self) {
    (void)env;
    (void)self;
    const uint32_t ticket = __atomic_add_fetch(&overlap_count, 1, __ATOMIC_SEQ_CST);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (__atomic_load_n(&overlap_count, __ATOMIC_SEQ_CST) < 2) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL +
                                now.tv_nsec - start.tv_nsec;
        if (elapsed > 2000000000LL) return -1;
        sched_yield();
    }
    return (int32_t)ticket;
}

API int32_t zb_probe_tid(uint32_t env, uint32_t self) {
    (void)env;
    (void)self;
    return (int32_t)syscall(__NR_gettid);
}
```

- [ ] **Step 2: Add the failing service-runtime test and public contract**

Create `tests/host/library_runtime_test.cpp` from the complete test body in Step 4 and
add its CMake block before adding the runtime header or source. Run:

```sh
tools/build_guest.sh
ninja -C build/host library_runtime_test
```

Expected: compilation fails because `zb/library_runtime.h` does not exist. This is the
RED proof for the task. Then add the public header below; rebuilding must progress to
an unresolved implementation failure before Step 3.

`library_runtime.h` exposes explicit lifetime and does not mention JNI:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zb/guest_thread.h"
#include "zb/native_call.h"

namespace zb {

class LibraryRuntime {
public:
    class Carrier;

    LibraryRuntime();
    ~LibraryRuntime();
    LibraryRuntime(const LibraryRuntime&) = delete;
    LibraryRuntime& operator=(const LibraryRuntime&) = delete;

    bool start(const std::string& zbhost, const std::string& sysroot,
               std::uint32_t target_sdk, const std::vector<std::string>& envp,
               std::string& error);
    std::uint32_t load_library(const std::string& path, int flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name,
                              std::string& error);
    std::optional<GuestResult> call_on_service(std::uint32_t function,
                                               const GuestCall& args);
    std::unique_ptr<Carrier> borrow(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
```

- [ ] **Step 3: Implement the ready call and command queue**

`LibraryRuntime::Impl` owns one `Process`, one joinable boot thread, a mutex, condition
variables, and a FIFO of commands. `start` passes `target_sdk` as the sole guest
argument and installs `Process::set_host_call_handler` before launching
`Process::run` on the boot thread. The handler for
`ZB_SERVICE_READY_INDEX` must:

1. Read and validate the complete `zb_service_api` through `GuestMemory::host_ptr`.
2. Publish readiness and retain the service `GuestThread*` only while its handler is active.
3. Wait for one command at a time.
4. Execute guest helpers only through `Process::call_guest` on that same thread.
5. Complete each command's promise before waiting again.
6. On shutdown, return zero in `r0`, leave the ready handler, and join `Process::run`.

Task 4 commands are `Load`, `Symbol`, and `Call`; Task 5 adds `SpawnCarrier`. `Load`
and `Symbol` copy their string to the validated scratch buffer, rejecting embedded NUL
or strings that do not fit. `Load` uses the caller's flags. A zero result calls
`dlerror_fn`, reads at most 4096 accessible guest bytes through `GuestMemory`, and
reports that text.

All public methods fail cleanly when startup failed, shutdown began, the protocol is
wrong, a helper call failed, or the guest process exited. They never hold the queue
mutex while executing translated guest code.

Use this complete `core/src/library_runtime.cpp` for the service-only implementation;
Task 5 extends the same `Impl` with carrier records without changing command semantics:

```cpp
#include "zb/library_runtime.h"

#include <cstring>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "zb/library_protocol.h"
#include "zb/process.h"

namespace zb {

namespace {

struct Response {
    bool ok = false;
    std::optional<GuestResult> result;
    std::string error;
};

struct Command {
    enum class Kind { Load, Symbol, Call };
    Kind kind;
    std::uint32_t value = 0;
    int flags = 0;
    std::string text;
    GuestCall args;
    std::promise<Response> done;
};

}  // namespace

struct LibraryRuntime::Impl {
    Process process;
    std::thread runner;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Command>> commands;
    zb_service_api api{};
    bool started = false;
    bool ready = false;
    bool stopping = false;
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
            candidate.spawn_carrier_fn == 0 || candidate.scratch_size == 0 ||
            candidate.scratch_size > ZB_SERVICE_SCRATCH_SIZE ||
            process.memory().host_ptr(candidate.scratch, candidate.scratch_size, kPageWrite) == nullptr) {
            error = "zbhost passed an invalid service API";
            return false;
        }
        api = candidate;
        return true;
    }

    bool copy_text(const std::string& text, std::string& error) {
        if (text.find('\0') != std::string::npos || text.size() + 1 > api.scratch_size) {
            error = "guest service string does not fit its scratch buffer";
            return false;
        }
        std::uint8_t* destination = process.memory().host_ptr(api.scratch, text.size() + 1, kPageWrite);
        if (destination == nullptr) {
            error = "guest service scratch buffer became unreadable";
            return false;
        }
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    }

    std::string read_text(std::uint32_t address) {
        if (address == 0) return "guest dlerror returned null";
        std::string text;
        for (std::size_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
            const std::uint8_t* byte = process.memory().host_ptr(address + i, 1, kPageRead);
            if (byte == nullptr) return "guest dlerror returned an unreadable string";
            if (*byte == 0) return text;
            text.push_back(static_cast<char>(*byte));
        }
        return "guest dlerror string is not terminated";
    }

    std::string last_dlerror(GuestThread& thread) {
        const auto result = process.call_guest(thread, api.dlerror_fn, GuestCall{});
        return result ? read_text(result->r0) : "guest dlerror call failed";
    }

    Response execute(GuestThread& thread, const Command& command) {
        GuestCall args = command.args;
        std::uint32_t function = command.value;
        if (command.kind == Command::Kind::Load || command.kind == Command::Kind::Symbol) {
            Response failure;
            if (!copy_text(command.text, failure.error)) return failure;
            args.regs[1] = api.scratch;
            if (command.kind == Command::Kind::Load) {
                function = api.dlopen_fn;
                args.regs[0] = api.scratch;
                args.regs[1] = static_cast<std::uint32_t>(command.flags);
            } else {
                function = api.dlsym_fn;
                args.regs[0] = command.value;
            }
        }

        Response response;
        response.result = process.call_guest(thread, function, args);
        if (!response.result) {
            response.error = "guest service call failed";
            return response;
        }
        if ((command.kind == Command::Kind::Load || command.kind == Command::Kind::Symbol) &&
            response.result->r0 == 0) {
            response.error = last_dlerror(thread);
            return response;
        }
        response.ok = true;
        return response;
    }

    void fail_waiters(const std::string& error) {
        std::deque<std::shared_ptr<Command>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(commands);
        }
        for (const auto& command : pending) command->done.set_value(Response{false, std::nullopt, error});
    }

    bool handle_ready(GuestThread& thread) {
        std::string error;
        if (!validate_api(thread.regs()[0], error)) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                startup_error = std::move(error);
            }
            thread.regs()[0] = 1;
            cv.notify_all();
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        cv.notify_all();

        for (;;) {
            std::shared_ptr<Command> command;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stopping || !commands.empty(); });
                if (stopping && commands.empty()) break;
                command = commands.front();
                commands.pop_front();
            }
            command->done.set_value(execute(thread, *command));
            if (process.exiting()) break;
        }
        thread.regs()[0] = 0;
        return true;
    }

    bool handle_host_call(std::uint32_t index, GuestThread& thread) {
        return index == ZB_SERVICE_READY_INDEX && handle_ready(thread);
    }

    Response submit(std::shared_ptr<Command> command) {
        std::future<Response> future = command->done.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || stopping || finished) {
                return {false, std::nullopt, "guest library runtime is not running"};
            }
            commands.push_back(std::move(command));
        }
        cv.notify_all();
        return future.get();
    }
};

LibraryRuntime::LibraryRuntime() : impl_(std::make_unique<Impl>()) {}

LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    impl_->runner.join();
}

bool LibraryRuntime::start(const std::string& zbhost, const std::string& sysroot,
                           std::uint32_t target_sdk, const std::vector<std::string>& envp,
                           std::string& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) {
            error = "guest library runtime was already started";
            return false;
        }
        impl_->started = true;
    }
    impl_->process.set_sysroot(sysroot);
    impl_->process.set_host_call_handler(
        [this](std::uint32_t index, GuestThread& thread) { return impl_->handle_host_call(index, thread); });
    impl_->runner = std::thread([this, zbhost, target_sdk, envp] {
        const int status = impl_->process.run(zbhost, {zbhost, std::to_string(target_sdk)}, envp);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->finished = true;
            if (!impl_->ready && impl_->startup_error.empty()) {
                impl_->startup_error = "zbhost exited with status " + std::to_string(status);
            }
        }
        impl_->fail_waiters("zbhost exited");
        impl_->cv.notify_all();
    });

    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait(lock, [&] { return impl_->ready || impl_->finished || !impl_->startup_error.empty(); });
    if (!impl_->ready) {
        error = impl_->startup_error;
        return false;
    }
    return true;
}

std::uint32_t LibraryRuntime::load_library(const std::string& path, int flags,
                                           std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Load;
    command->text = path;
    command->flags = flags;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::uint32_t LibraryRuntime::find_symbol(std::uint32_t handle, const std::string& name,
                                          std::string& error) {
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

std::optional<GuestResult> LibraryRuntime::call_on_service(std::uint32_t function,
                                                           const GuestCall& args) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Call;
    command->value = function;
    command->args = args;
    Response response = impl_->submit(std::move(command));
    return response.ok ? response.result : std::nullopt;
}

}  // namespace zb
```

- [ ] **Step 4: Re-run the end-to-end service-thread test**

`library_runtime_test` receives `sysroot`, `zbhost`, and `libzbcallprobe.so`. Start the
runtime with target SDK 16 and `LD_LIBRARY_PATH=<build/guest/lib>`, assert that a
nonexistent library returns a guest `dlerror`, load the probe, resolve every export,
and call all return-only functions through `call_on_service`. Also verify
missing-symbol diagnostics.

```cpp
#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "check.h"
#include "zb/library_runtime.h"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string libdir = std::filesystem::path(argv[3]).parent_path();
    zb::LibraryRuntime runtime;
    std::string error;
    CHECK(runtime.start(argv[2], argv[1], 16, {"LD_LIBRARY_PATH=" + libdir}, error));

    CHECK(runtime.load_library(libdir + "/does-not-exist.so", RTLD_NOW, error) == 0);
    CHECK(!error.empty());
    error.clear();
    const std::uint32_t library = runtime.load_library(argv[3], RTLD_NOW | RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());
    CHECK(runtime.find_symbol(library, "does_not_exist", error) == 0 && !error.empty());

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    const auto call = [&](const char* name) {
        const auto result = runtime.call_on_service(symbol(name), zb::GuestCall{});
        CHECK(result);
        return *result;
    };

    (void)call("zb_return_v");
    CHECK(call("zb_return_z").r0 == 1);
    CHECK(call("zb_return_b").r0 == 0xFFFFFFFEu);
    CHECK(call("zb_return_c").r0 == 0x1234);
    CHECK(call("zb_return_s").r0 == 0xFFFFFFFDu);
    CHECK(call("zb_return_i").r0 == 42);
    const zb::GuestResult j = call("zb_return_j");
    CHECK((static_cast<std::uint64_t>(j.r1) << 32 | j.r0) == 0x1122334455667788ull);
    CHECK(call("zb_return_f").r0 == fbits(3.5f));
    const zb::GuestResult d = call("zb_return_d");
    CHECK((static_cast<std::uint64_t>(d.r1) << 32 | d.r0) == dbits(-1.25));
    CHECK(call("zb_return_l").r0 == 0x12345);

    std::puts("library_runtime_test service PASS");
    return 0;
}
```

Declare the test separately so it receives:

```cmake
add_executable(library_runtime_test library_runtime_test.cpp)
target_link_libraries(library_runtime_test PRIVATE zbcore)
add_test(NAME library_runtime_test COMMAND library_runtime_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib/libzbcallprobe.so)
```

Add `src/library_runtime.cpp` to the `zbcore` source list in `core/CMakeLists.txt`.

- [ ] **Step 5: Verify and commit**

```sh
tools/build_guest.sh
ninja -C build/host library_runtime_test
ctest --test-dir build/host -R '^library_runtime_test$' --output-on-failure
ninja -C build/host
ctest --test-dir build/host --output-on-failure
git status --short
git add core/include/zb/library_runtime.h core/src/library_runtime.cpp core/CMakeLists.txt \
    guest/testlib/zbcallprobe.c tests/host/library_runtime_test.cpp \
    tools/build_guest.sh tests/host/CMakeLists.txt
git commit -m "core: load and call guest libraries through zbhost"
```

---

## Task 5: Carrier leases and guest-tid routing

**Files:**
- Modify: `core/include/zb/process.h`, `core/src/process.cpp`
- Modify: `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp`
- Extend: `tests/host/library_runtime_test.cpp`

**Interfaces:** `Process::create_borrower`, `Process::destroy_borrower`,
`LibraryRuntime::borrow`, `LibraryRuntime::Carrier`.

- [ ] **Step 1: Add and run the failing two-thread test**

Two host threads each call `runtime.borrow`, wait on a `std::barrier`, marshal
`"IZBCSIJFDL"` with `marshal_native_args`, and call `zb_probe_mix`. Each also calls
`zb_probe_tls`. Assert:

- both results are 42;
- both TLS values are nonzero and different;
- the calls rendezvous inside `zb_probe_overlap`, proving they execute concurrently;
- releasing both leases lets guest pthread cleanup complete;
- a later borrow succeeds, proving the carrier path is reusable.

This must fail before carrier handling exists; accepting serialized service calls does
not satisfy the test.

Replace `library_runtime_test.cpp` with this complete test:

```cpp
#include <dlfcn.h>

#include <array>
#include <barrier>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "check.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace {

std::uint64_t fbits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t dbits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint32_t to_handle(std::uint64_t reference) {
    return reference == 0 ? 0 : static_cast<std::uint32_t>(reference + 0x1000);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string libdir = std::filesystem::path(argv[3]).parent_path();
    zb::LibraryRuntime runtime;
    std::string error;
    CHECK(runtime.start(argv[2], argv[1], 16, {"LD_LIBRARY_PATH=" + libdir}, error));

    CHECK(runtime.load_library(libdir + "/does-not-exist.so", RTLD_NOW, error) == 0);
    CHECK(!error.empty());
    error.clear();
    const std::uint32_t library = runtime.load_library(argv[3], RTLD_NOW | RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    const auto service_call = [&](const char* name) {
        const auto result = runtime.call_on_service(symbol(name), zb::GuestCall{});
        CHECK(result);
        return *result;
    };
    (void)service_call("zb_return_v");
    CHECK(service_call("zb_return_z").r0 == 1);
    CHECK(service_call("zb_return_b").r0 == 0xFFFFFFFEu);
    CHECK(service_call("zb_return_c").r0 == 0x1234);
    CHECK(service_call("zb_return_s").r0 == 0xFFFFFFFDu);
    CHECK(service_call("zb_return_i").r0 == 42);
    const zb::GuestResult j = service_call("zb_return_j");
    CHECK((static_cast<std::uint64_t>(j.r1) << 32 | j.r0) == 0x1122334455667788ull);
    CHECK(service_call("zb_return_f").r0 == fbits(3.5f));
    const zb::GuestResult d = service_call("zb_return_d");
    CHECK((static_cast<std::uint64_t>(d.r1) << 32 | d.r0) == dbits(-1.25));
    CHECK(service_call("zb_return_l").r0 == 0x12345);

    const std::uint32_t mix = symbol("zb_probe_mix");
    const std::uint32_t tls_function = symbol("zb_probe_tls");
    const std::uint32_t tid_function = symbol("zb_probe_tid");
    const std::uint32_t overlap = symbol("zb_probe_overlap");
    std::barrier rendezvous(2);
    std::array<std::uint32_t, 2> tls{};
    std::array<std::int32_t, 2> tids{};
    std::array<std::int32_t, 2> tickets{};

    const auto worker = [&](std::size_t index) {
        std::string borrow_error;
        auto carrier = runtime.borrow(borrow_error);
        CHECK(carrier && borrow_error.empty());

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
        const zb::GuestCall mixed = zb::marshal_native_args("IZBCSIJFDL", host, 0xE000, to_handle);
        const auto mixed_result = carrier->call(mix, mixed);
        CHECK(mixed_result && mixed_result->r0 == 42);

        zb::GuestCall identity;
        identity.regs = {0xE000, 0x1077, 0, 0};
        const auto tls_result = carrier->call(tls_function, identity);
        const auto tid_result = carrier->call(tid_function, identity);
        CHECK(tls_result && tid_result);
        tls[index] = tls_result->r0;
        tids[index] = static_cast<std::int32_t>(tid_result->r0);
        CHECK(tids[index] == carrier->guest_tid());
        CHECK(tls[index] == carrier->guest_tls());

        rendezvous.arrive_and_wait();
        const auto overlap_result = carrier->call(overlap, identity);
        CHECK(overlap_result);
        tickets[index] = static_cast<std::int32_t>(overlap_result->r0);
    };

    std::thread first(worker, 0);
    std::thread second(worker, 1);
    first.join();
    second.join();
    CHECK(tls[0] != 0 && tls[1] != 0 && tls[0] != tls[1]);
    CHECK(tids[0] > 0 && tids[1] > 0 && tids[0] != tids[1]);
    CHECK(tickets[0] > 0 && tickets[1] > 0 && tickets[0] != tickets[1]);

    error.clear();
    auto later = runtime.borrow(error);
    CHECK(later && error.empty());
    zb::GuestCall identity;
    identity.regs = {0xE000, 0x1077, 0, 0};
    CHECK(later->call(tls_function, identity));
    later.reset();

    std::puts("library_runtime_test PASS");
    return 0;
}
```

- [ ] **Step 2: Add a borrowed-JIT registry to Process**

Add these APIs:

```cpp
std::unique_ptr<GuestThread> create_borrower(std::uint32_t tls, std::uint32_t stack,
                                             std::int32_t guest_tid);
void destroy_borrower(std::unique_ptr<GuestThread> thread);
```

`create_borrower` allocates a processor id, creates a JIT, sets TLS, aligned SP, user
CPSR, and `tid = guest_tid`, then records it in `borrowers_` under `threads_mutex_`.
`destroy_borrower` erases it before clearing the monitor processor id. `invalidate`
visits both `threads_` and `borrowers_`. `thread_count` continues to count only real
guest pthreads. `find_thread` searches a borrower with the requested tid before the
parked real carrier, so `tkill`/`tgkill` are redirected while borrowing.

The exact `process.cpp` additions are:

```cpp
std::unique_ptr<GuestThread> Process::create_borrower(std::uint32_t tls,
                                                       std::uint32_t stack,
                                                       std::int32_t guest_tid) {
    const int processor_id = allocate_processor_id();
    if (processor_id < 0) return nullptr;
    auto thread = std::make_unique<GuestThread>(mem_, monitor_.get(),
                                                static_cast<std::size_t>(processor_id),
                                                precise_faults_);
    thread->regs().fill(0);
    thread->regs()[13] = stack & ~7u;
    thread->ext_regs().fill(0);
    thread->set_cpsr(kCpsrUserMode);
    thread->set_tls(tls);
    thread->tid = guest_tid;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        borrowers_.push_back(thread.get());
    }
    return thread;
}

void Process::destroy_borrower(std::unique_ptr<GuestThread> thread) {
    if (!thread) return;
    const std::size_t processor_id = thread->processor_id();
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        std::erase(borrowers_, thread.get());
    }
    monitor_->ClearProcessor(processor_id);
    thread.reset();
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        processor_ids_.reset(processor_id);
    }
}
```

Replace the affected lookup/invalidation bodies with:

```cpp
GuestThread* Process::find_thread(std::int32_t tid) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* thread : borrowers_) {
        if (thread->tid == tid) return thread;
    }
    for (GuestThread* thread : threads_) {
        if (thread->tid == tid) return thread;
    }
    return nullptr;
}

void Process::invalidate(std::uint32_t addr, std::uint32_t len) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* thread : threads_) thread->invalidate(addr, len);
    for (GuestThread* thread : borrowers_) thread->invalidate(addr, len);
}
```

Add `std::vector<GuestThread*> borrowers_;` beside `threads_`. Add a
`Process::current_thread()` getter beside `set_current_thread`:

```cpp
static GuestThread* current_thread();
```

In `signals.cpp`, implement it against the existing thread-local pointer:

```cpp
GuestThread* Process::current_thread() {
    return t_current_thread;
}
```

- [ ] **Step 3: Define and implement the thread-bound Carrier**

Add to `LibraryRuntime::Carrier`:

```cpp
class LibraryRuntime::Carrier {
public:
    ~Carrier();
    Carrier(const Carrier&) = delete;
    Carrier& operator=(const Carrier&) = delete;
    Carrier(Carrier&&) = delete;
    Carrier& operator=(Carrier&&) = delete;

    std::optional<GuestResult> call(std::uint32_t function, const GuestCall& args);
    std::int32_t guest_tid() const;
    std::uint32_t guest_tls() const;

private:
    friend class LibraryRuntime;
    struct State;
    explicit Carrier(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
```

The carrier-park host-call handler creates a shared parked record containing the real
`GuestThread*`, its TLS, SP, and tid; publishes it to the waiter; then waits on that
record's condition variable. It does not return from the host-call handler while the
carrier is borrowed.

`borrow` first issues `SpawnCarrier` on the service thread. After guest
`pthread_create` returns success, it waits for one matching parked record, creates the
borrower JIT on the calling host thread, and returns a lease. `Carrier::call` sets
`Process::set_current_thread` for the duration and calls `Process::call_guest` directly
on the borrower JIT. Its destructor destroys the borrower JIT before marking the parked
record released and notifying the original carrier host thread.

The runtime must outlive every `Carrier`; enforce this with an active-lease count and a
fatal diagnostic in the runtime destructor rather than leaving dangling Process state.

Add `SpawnCarrier` to `Command::Kind` and this record in `library_runtime.cpp`:

```cpp
struct ParkedCarrier {
    GuestThread* thread = nullptr;
    std::uint32_t tls = 0;
    std::uint32_t stack = 0;
    std::int32_t tid = 0;
    bool released = false;
};
```

Extend `LibraryRuntime::Impl` with:

```cpp
std::deque<std::shared_ptr<ParkedCarrier>> parked;
std::size_t active_leases = 0;

bool park_carrier(GuestThread& thread) {
    auto record = std::make_shared<ParkedCarrier>();
    record->thread = &thread;
    record->tls = thread.tls();
    record->stack = thread.regs()[13];
    record->tid = thread.tid;
    std::unique_lock<std::mutex> lock(mutex);
    parked.push_back(record);
    cv.notify_all();
    cv.wait(lock, [&] { return record->released || stopping; });
    thread.regs()[0] = 0;
    return true;
}
```

The final host-call selector is:

```cpp
bool handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index == ZB_SERVICE_READY_INDEX) return handle_ready(thread);
    if (index == ZB_CARRIER_PARK_INDEX) return park_carrier(thread);
    return false;
}
```

The `SpawnCarrier` branch at the start of `execute` is:

```cpp
if (command.kind == Command::Kind::SpawnCarrier) {
    Response response;
    response.result = process.call_guest(thread, api.spawn_carrier_fn, GuestCall{});
    if (!response.result) {
        response.error = "guest pthread_create call failed";
    } else if (response.result->r0 != 0) {
        response.error = "guest pthread_create returned " + std::to_string(response.result->r0);
    } else {
        response.ok = true;
    }
    return response;
}
```

Define the private carrier state and methods in `library_runtime.cpp`:

```cpp
struct LibraryRuntime::Carrier::State {
    Impl* impl;
    std::shared_ptr<ParkedCarrier> parked;
    std::unique_ptr<GuestThread> borrower;
    std::thread::id owner;
};

LibraryRuntime::Carrier::Carrier(std::unique_ptr<State> state) : state_(std::move(state)) {}

LibraryRuntime::Carrier::~Carrier() {
    if (!state_) return;
    if (state_->owner != std::this_thread::get_id()) {
        log("carrier lease destroyed on a different host thread");
        std::abort();
    }
    Impl* impl = state_->impl;
    impl->process.destroy_borrower(std::move(state_->borrower));
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        state_->parked->released = true;
        --impl->active_leases;
    }
    impl->cv.notify_all();
}

std::optional<GuestResult> LibraryRuntime::Carrier::call(std::uint32_t function,
                                                         const GuestCall& args) {
    if (!state_ || state_->owner != std::this_thread::get_id()) {
        log("carrier lease called on a different host thread");
        std::abort();
    }
    GuestThread* previous = Process::current_thread();
    Process::set_current_thread(state_->borrower.get());
    const auto result = state_->impl->process.call_guest(*state_->borrower, function, args);
    Process::set_current_thread(previous);
    return result;
}

std::int32_t LibraryRuntime::Carrier::guest_tid() const {
    return state_ ? state_->parked->tid : 0;
}

std::uint32_t LibraryRuntime::Carrier::guest_tls() const {
    return state_ ? state_->parked->tls : 0;
}
```

Implement `borrow` exactly as follows:

```cpp
std::unique_ptr<LibraryRuntime::Carrier> LibraryRuntime::borrow(std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::SpawnCarrier;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return nullptr;
    }

    std::shared_ptr<ParkedCarrier> parked;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->cv.wait(lock, [&] { return !impl_->parked.empty() || impl_->finished || impl_->stopping; });
        if (impl_->parked.empty()) {
            error = "guest carrier did not park";
            return nullptr;
        }
        parked = impl_->parked.front();
        impl_->parked.pop_front();
    }

    auto borrower = impl_->process.create_borrower(parked->tls, parked->stack, parked->tid);
    if (!borrower) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            parked->released = true;
        }
        impl_->cv.notify_all();
        error = "no Dynarmic processor id is available for a carrier";
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->active_leases;
    }
    auto state = std::make_unique<Carrier::State>();
    state->impl = impl_.get();
    state->parked = std::move(parked);
    state->borrower = std::move(borrower);
    state->owner = std::this_thread::get_id();
    return std::unique_ptr<Carrier>(new Carrier(std::move(state)));
}
```

Replace the Task 4 destructor with:

```cpp
LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->active_leases != 0) {
            log("library runtime destroyed with active carrier leases");
            std::abort();
        }
        impl_->stopping = true;
        for (const auto& record : impl_->parked) record->released = true;
    }
    impl_->cv.notify_all();
    impl_->runner.join();
}
```

Add `<cstdlib>` and `zb/log.h` to `library_runtime.cpp` for the fatal ownership and
lifetime checks.

- [ ] **Step 4: Verify concurrency and commit**

```sh
tools/build_guest.sh
ninja -C build/host library_runtime_test
ctest --test-dir build/host -R '^library_runtime_test$' --repeat until-fail:50 \
    --output-on-failure
ninja -C build/host
ctest --test-dir build/host --output-on-failure
git status --short
git add core/include/zb/process.h core/src/process.cpp core/include/zb/library_runtime.h \
    core/src/library_runtime.cpp tests/host/library_runtime_test.cpp
git commit -m "core: run guest calls on borrowed carrier threads"
```

If the configured Dynarmic build supports ThreadSanitizer, also run the focused test
under that build. Record an unsupported sanitizer build in the task ledger rather
than silently skipping it.

---

## Task 6: Regression, Android link, and status docs

**Files:**
- Modify: `CLAUDE.md`
- Modify: `AGENTS.md`

**Interfaces:** no code interface changes; this task verifies and records the Phase 4b
contract.

- [ ] **Step 1: Run all local checks**

```sh
tools/build_guest.sh
ninja -C build/host
ctest --test-dir build/host --output-on-failure
tools/run_guest_tests.sh
cmake -S . -B build/android-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/home/Zailox/android-ndk-r29/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DZB_BUILD_TESTS=OFF -DBoost_INCLUDE_DIR=/home/Zailox/ZettaBridge/build/boost-headers
ninja -C build/android-arm64 zbridge zbrun
```

Expected: all existing and new host tests pass, all 9 legacy guest cases pass, and both
Android arm64 targets link.

- [ ] **Step 2: Review invariants before closing 4b**

- Every call path goes through `dispatch_stop`; no duplicated syscall loop exists.
- `kHostReturnSwi` is consumed only by an active nested call.
- A service JIT is touched only by its boot thread.
- A borrower JIT is created, called, and destroyed on the borrowing host thread.
- A parked carrier cannot resume before its borrower is removed from tid routing.
- Runtime shutdown cannot race an active lease or queued promise.
- `third_party/dynarmic` is not staged.

- [ ] **Step 3: Update handoff docs and commit**

Mark Phase 4b complete and set plan 4c (generated guest `JNIEnv`, host JNI backend, and
mock JNI test) as next. Commit:

```sh
git commit -m "docs: mark Phase 4b complete"
```

---

## Acceptance

Phase 4b is complete only when a host test boots the real arm32 linker and `zbhost`,
loads `libzbcallprobe.so`, resolves its exports, marshals every JNI shorty category,
and executes calls concurrently from two host threads on two distinct carrier TLS/tid
identities. The complete pre-4b host and guest suites must remain green.

## Following plan boundaries

- **4c** consumes `LibraryRuntime::Carrier`, handle tables, shorties, and `GuestCall`.
  It generates `libzbjni.so`, implements the flat host JNI backend, and runs the mock
  JNI suite without ART.
- **4d** owns ART and launcher integration: proxy libraries, `onProxyLoaded`, export
  binding, per-method `RegisterNatives`, carrier caching in a host pthread-key
  destructor, T7, and the Orange Roulette Phase 4 smoke test.
