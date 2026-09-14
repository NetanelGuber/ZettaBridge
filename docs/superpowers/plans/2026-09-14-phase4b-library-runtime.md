# Phase 4b: Library Runtime, Host-to-Guest Calls, and Carriers

Status: draft for review 2026-09-14.

## Goal

Turn the Phase 1-3 executable runner into a reusable library-mode guest process. The
host must be able to load an arm32 shared library, resolve a symbol, and call it either
on the service guest thread or on a carrier borrowed by the current host thread.

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

Commit locally after every task. Never push.

---

## Task 1: Nested host-to-guest call frame

**Files:**
- Modify: `core/include/zb/guest_thread.h`
- Modify: `core/src/guest_thread.cpp`
- Create: `tests/host/guest_call_test.cpp`
- Modify: `tests/host/CMakeLists.txt`

### Step 1: Add the failing host test

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

### Step 2: Add the call contract

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

### Step 3: Implement the call frame

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

Run the focused test, the full host suite, and commit:

```sh
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

### Step 1: Add a failing dispatch test

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
using HostCallHandler = std::function<bool(std::uint32_t index, GuestThread& thread)>;
void set_host_call_handler(HostCallHandler handler) { host_call_handler_ = std::move(handler); }
std::optional<GuestResult> call_guest(GuestThread& thread, std::uint32_t target,
                                      const GuestCall& args);
```

Add `host_call_dispatch_test` as a separately declared test, like `elf_loader_test`,
with `${CMAKE_SOURCE_DIR}/build/guest/host_call_static` as its argument.

### Step 2: Extract one stop dispatcher

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
std::optional<GuestResult> Process::call_guest(GuestThread& thread, std::uint32_t target,
                                               const GuestCall& args) {
    return thread.call(target, args, [&](const Stop& stop) {
        if (!dispatch_stop(thread, stop)) return false;
        return !thread.has_pending_signals(thread.sigmask) || dispatch_pending_signals(thread);
    });
}
```

Do not special-case syscalls made during nested calls. A guest `exit` or fatal signal
fails the call through the same path as the main loop.

### Step 3: Make guest tid explicit

Change `NR_gettid` and the return from `NR_set_tid_address` to use `thread.tid` when it
is nonzero. Normal guest pthreads already store their host tid, so this is behaviorally
unchanged until carriers arrive:

```cpp
const auto guest_tid = [&] {
    return thread.tid != 0 ? thread.tid : static_cast<std::int32_t>(::syscall(SYS_gettid));
};
```

Run `tools/build_guest.sh`, the focused test, the full host suite, and commit:

```sh
git commit -m "core: reuse stop dispatch for nested guest calls"
```

---

## Task 3: Fixed `zbhost` service protocol

**Files:**
- Create: `core/include/zb/library_protocol.h`
- Create: `guest/zbhost/zbhost.c`
- Modify: `tools/build_guest.sh`

### Step 1: Define the C-compatible handshake

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

Build it as `build/guest/zbhost` with the same dynamic PIE toolchain as the existing
dynamic tests and `-Icore/include -ldl`. Verify with `readelf` that it is ELF32 ARM,
has `/system/bin/linker`, and has no undefined ZettaBridge symbol.

Run the existing guest suite and commit:

```sh
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

### Step 1: Add the softfp probe library

Build `zbcallprobe.c` as `libzbcallprobe.so`. Every exported function uses
`__attribute__((pcs("aapcs")))` so its public boundary is explicitly base AAPCS even
if a future toolchain default changes.

```c
#include <stdint.h>
#include <time.h>

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

API int32_t zb_probe_sleep(uint32_t env, uint32_t self, int32_t milliseconds) {
    (void)env;
    (void)self;
    const struct timespec delay = {0, milliseconds * 1000000L};
    return nanosleep(&delay, NULL);
}
```

### Step 2: Add the public runtime contract

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

### Step 3: Implement the ready call and command queue

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

Commands are `Load`, `Symbol`, `Call`, `SpawnCarrier`, and `Stop`. `Load` and `Symbol`
copy their string to the validated scratch buffer, rejecting embedded NUL or strings
that do not fit. `Load` uses the caller's flags. A zero result calls `dlerror_fn`, reads
at most 4096 accessible guest bytes through `GuestMemory`, and reports that text.

All public methods fail cleanly when startup failed, shutdown began, the protocol is
wrong, a helper call failed, or the guest process exited. They never hold the queue
mutex while executing translated guest code.

### Step 4: First end-to-end test on the service thread

`library_runtime_test` receives `sysroot`, `zbhost`, and `libzbcallprobe.so`. Start the
runtime with target SDK 16 and `LD_LIBRARY_PATH=<build/guest/lib>`, assert that a
nonexistent library returns a guest `dlerror`, load the probe, resolve every export,
and call all return-only functions through `call_on_service`. Also verify
missing-symbol diagnostics.

Run `tools/build_guest.sh`, the focused test, the full host suite, and commit:

```sh
git commit -m "core: load and call guest libraries through zbhost"
```

---

## Task 5: Carrier leases and guest-tid routing

**Files:**
- Modify: `core/include/zb/process.h`, `core/src/process.cpp`
- Modify: `core/include/zb/library_runtime.h`, `core/src/library_runtime.cpp`
- Extend: `tests/host/library_runtime_test.cpp`

### Step 1: Add the failing two-thread test

Two host threads each call `runtime.borrow`, wait on a `std::barrier`, marshal
`"IZBCSIJFDL"` with `marshal_native_args`, and call `zb_probe_mix`. Each also calls
`zb_probe_tls`. Assert:

- both results are 42;
- both TLS values are nonzero and different;
- the calls overlap in wall time after adding a short guest `nanosleep` probe;
- releasing both leases lets guest pthread cleanup complete;
- a later borrow succeeds, proving the carrier path is reusable.

This must fail before carrier handling exists; accepting serialized service calls does
not satisfy the test.

### Step 2: Add a borrowed-JIT registry to Process

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

### Step 3: Define the move-only Carrier

Add to `LibraryRuntime::Carrier`:

```cpp
class LibraryRuntime::Carrier {
public:
    Carrier(Carrier&&) noexcept;
    Carrier& operator=(Carrier&&) noexcept;
    ~Carrier();
    Carrier(const Carrier&) = delete;
    Carrier& operator=(const Carrier&) = delete;

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

Run the focused test repeatedly (`--repeat until-fail:50`), ThreadSanitizer if the
Dynarmic build supports it, the full host suite, and commit:

```sh
git commit -m "core: run guest calls on borrowed carrier threads"
```

---

## Task 6: Regression, Android link, and status docs

**Files:**
- Modify: `CLAUDE.md`
- Modify: `AGENTS.md`

### Step 1: Run all local checks

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

### Step 2: Review invariants before closing 4b

- Every call path goes through `dispatch_stop`; no duplicated syscall loop exists.
- `kHostReturnSwi` is consumed only by an active nested call.
- A service JIT is touched only by its boot thread.
- A borrower JIT is created, called, and destroyed on the borrowing host thread.
- A parked carrier cannot resume before its borrower is removed from tid routing.
- Runtime shutdown cannot race an active lease or queued promise.
- `third_party/dynarmic` is not staged.

### Step 3: Update handoff docs and commit

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
