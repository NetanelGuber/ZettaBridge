# Phase 1: zbrun + Dynarmic (T1-T2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `core/` + `zbrun`, a Linux aarch64 CLI that runs static arm32 Android
executables through Dynarmic. Accept on T1 (hand-written blob) and T2 (static NDK hello).

**Architecture:** Follows `docs/superpowers/specs/2026-09-13-guest-system-boundary-design.md`.
- **Memory:** one 4 GiB guest reservation used as Dynarmic fastmem, with a per-page flag
  table.
- **Threads:** `GuestThread` wraps one `Dynarmic::A32::Jit`. Its callbacks only record a
  stop and halt the JIT; syscalls run after `Run()` returns.
- **Loading:** an ELF32 loader and a kernel-style initial stack.
- **Syscalls:** the EABI syscall layer maps guest arguments onto host syscalls, converting
  32-bit structs.

**Tech Stack:** C++20, CMake + Ninja, system clang (host); NDK r29
`armv7a-linux-androideabi21-clang` (guest tests); Dynarmic (Vita3K fork, pinned
`86458a0b`, git submodule `third_party/dynarmic`).

**Execution note:** This plan is executed inline by the same agent that wrote it, in an
unattended session. Interfaces, tests, commands and non-obvious algorithms are given
in full. Straightforward bodies (one host syscall per guest syscall) are specified by
exact semantics instead of duplicated code.

**Verified inputs:**
- Dynarmic builds on this host in ~50 s with `DYNARMIC_FRONTENDS=A32`,
  `DYNARMIC_USE_BUNDLED_EXTERNALS=ON`, system Boost 1.83.
- After `svc`, the A32 translator has already written PC = next instruction.
- The arm64 backend dereferences `conf.global_monitor` for exclusive ops, so a monitor
  is mandatory.
- `HaltExecution` ORs into an atomic flag; `ClearHalt` clears it.
- NDK syscall numbers: `.../sysroot/usr/include/arm-linux-androideabi/asm/unistd-eabi.h`,
  lines of the form `#define __NR_x (__NR_SYSCALL_BASE + N)`.

---

## File structure

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | top-level: Dynarmic subproject, `core`, `cli/zbrun`, host tests |
| `core/CMakeLists.txt` | `zbcore` static library |
| `core/include/zb/log.h`, `core/src/log.cpp` | `zb::log(fmt, ...)` to stderr |
| `core/include/zb/guest_memory.h`, `core/src/guest_memory.cpp` | 4 GiB reservation, page flags, map/protect/unmap/find_free, checked host pointers |
| `core/include/zb/cp15.h`, `core/src/cp15.cpp` | CP15 coprocessor: TPIDRURO / TPIDRURW |
| `core/include/zb/guest_thread.h`, `core/src/guest_thread.cpp` | `GuestThread`: Jit + callbacks, `run()` returns a `Stop` |
| `core/include/zb/elf_loader.h`, `core/src/elf_loader.cpp` | load ARM ELF32 ET_EXEC/ET_DYN into guest memory |
| `core/include/zb/initial_stack.h`, `core/src/initial_stack.cpp` | argv/envp/auxv stack |
| `core/include/zb/guest_abi.h` | explicit 32-bit guest struct layouts with static asserts |
| `core/include/zb/syscalls.h`, `core/src/syscalls.cpp` | EABI syscall dispatcher |
| `core/src/gen/syscall_nrs_arm.h`, `core/src/gen/syscall_names_arm.inc` | generated from NDK header, committed |
| `core/include/zb/process.h`, `core/src/process.cpp` | `Process`: memory, monitor, brk, signal store, main loop, crash report |
| `cli/zbrun/CMakeLists.txt`, `cli/zbrun/main.cpp` | CLI |
| `tests/host/CMakeLists.txt`, `tests/host/check.h`, `tests/host/*_test.cpp` | host unit tests (ctest) |
| `tools/gen_syscalls.py` | generator for `core/src/gen/*` |
| `tools/abi_check.c` | compiled by NDK only; `_Static_assert`s real bionic layouts equal `guest_abi.h` |
| `tools/build_guest.sh` | builds guest test binaries with NDK |
| `tools/run_guest_tests.sh` | runs guest binaries under zbrun, diffs stdout + exit code |
| `guest/tests/hello_static.c`, `guest/tests/expected/hello_static.out` | T2 |

Build (host):
```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; ninja -C build/host; ctest --test-dir build/host --output-on-failure
```
Guest tests:
```
tools/build_guest.sh; tools/run_guest_tests.sh
```

---

### Task 1: CMake skeleton + log + Dynarmic link check

**Files:** Create `CMakeLists.txt`, `core/CMakeLists.txt`, `core/include/zb/log.h`,
`core/src/log.cpp`, `cli/zbrun/CMakeLists.txt`, `cli/zbrun/main.cpp` (temporary:
constructs a `Dynarmic::ExclusiveMonitor(1)` and prints `zbrun ok`),
`tests/host/CMakeLists.txt`, `tests/host/check.h`.

- [ ] Top-level CMake:
```cmake
cmake_minimum_required(VERSION 3.22)
project(zettabridge LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)
add_subdirectory(core)
add_subdirectory(cli/zbrun)
option(ZB_BUILD_TESTS "Build host tests" ON)
if (ZB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/host)
endif()
```
- [ ] `tests/host/check.h`:
```cpp
#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)
```
- [ ] Build: the host build command above. Expected: links; `build/host/cli/zbrun/zbrun` prints `zbrun ok`.
- [ ] Commit `build: CMake skeleton linking Dynarmic`.

### Task 2: GuestMemory (TDD)

**Files:** `core/include/zb/guest_memory.h`, `core/src/guest_memory.cpp`, `tests/host/guest_memory_test.cpp`.

Interface:
```cpp
namespace zb {
inline constexpr std::uint64_t kGuestSpaceSize = 1ULL << 32;
inline constexpr std::uint32_t kPageSize = 4096;
inline constexpr std::uint32_t kPageMask = kPageSize - 1;
enum PageFlags : std::uint8_t { kPageRead = 1, kPageWrite = 2, kPageExec = 4, kPageMapped = 8 };
std::uint64_t page_round_up(std::uint64_t v);
std::uint32_t page_round_down(std::uint32_t v);
int host_prot(int guest_prot);   // READ if guest R or X (the JIT reads code), WRITE -> READ|WRITE
class GuestMemory {
public:
    GuestMemory();                                  // mmap PROT_NONE 4 GiB + 64 KiB guard, MAP_NORESERVE
    bool ok() const;
    std::uint8_t* base() const;
    bool map_anon(std::uint32_t addr, std::uint64_t len, int prot);                 // MAP_FIXED zeroed
    bool map_file(std::uint32_t addr, std::uint64_t len, int prot, int share_flags, int fd, std::uint64_t offset);
    bool protect(std::uint32_t addr, std::uint64_t len, int prot);                  // fails if any page unmapped
    bool unmap(std::uint32_t addr, std::uint64_t len);                              // back to PROT_NONE reservation
    std::uint32_t find_free(std::uint64_t len, std::uint32_t limit) const;         // highest fit ending <= limit, never below 0x10000; 0 = none
    bool range_free(std::uint32_t addr, std::uint64_t len) const;
    bool accessible(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const; // mapped + all need bits, no wrap past 4 GiB
    std::uint8_t* host_ptr(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const;
    std::uint8_t page_flags(std::uint32_t addr) const;
};
}
```
Rules: addresses must be page-aligned (else return false); lengths are rounded up to
pages; `addr + len > 4 GiB` returns false. The page table is `std::vector<uint8_t>(1 << 20)`.

- [ ] Write `tests/host/guest_memory_test.cpp`:
```cpp
#include "check.h"
#include "zb/guest_memory.h"
#include <sys/mman.h>
int main() {
    zb::GuestMemory m;
    CHECK(m.ok());
    CHECK(!m.accessible(0, 1, zb::kPageRead));
    CHECK(m.map_anon(0x10000, 5000, PROT_READ | PROT_WRITE));
    CHECK(m.accessible(0x10000, 8192, zb::kPageRead | zb::kPageWrite));
    CHECK(!m.accessible(0x10000, 8193, zb::kPageRead));
    m.base()[0x11fff] = 7;
    CHECK(m.host_ptr(0x11fff, 1, zb::kPageRead)[0] == 7);
    CHECK(m.protect(0x10000, 4096, PROT_READ | PROT_EXEC));
    CHECK(m.host_ptr(0x10000, 1, zb::kPageWrite) == nullptr);
    CHECK(m.accessible(0x10000, 4, zb::kPageExec));
    CHECK(!m.protect(0x20000, 4096, PROT_READ));
    CHECK(!m.map_anon(0x10001, 10, PROT_READ));
    CHECK(!m.accessible(0xFFFFFFF0, 32, 0));
    std::uint32_t f = m.find_free(3 * 4096, 0x40000000);
    CHECK(f != 0 && (f & 0xFFF) == 0 && f + 3 * 4096 <= 0x40000000);
    CHECK(m.range_free(f, 3 * 4096));
    CHECK(m.unmap(0x10000, 8192));
    CHECK(!m.accessible(0x10000, 1, 0));
    CHECK(m.find_free(4096, 0x11000) == 0x10000);
    std::puts("guest_memory_test PASS");
}
```
- [ ] Add to `tests/host/CMakeLists.txt` a `foreach` over test names: `add_executable`,
  link `zbcore`, `add_test`. Run ctest. Expected: FAIL (does not build).
- [ ] Implement. Run ctest. Expected: `guest_memory_test PASS`.
- [ ] Commit `core: GuestMemory 4 GiB reservation with page flags`.

### Task 3: CP15 + GuestThread + T1 (TDD)

**Files:** `core/include/zb/cp15.h`, `core/src/cp15.cpp`, `core/include/zb/guest_thread.h`,
`core/src/guest_thread.cpp`, `tests/host/t1_blob_test.cpp`.

Interface:
```cpp
namespace zb {
inline constexpr std::uint32_t kHostReturnSwi = 0x5AFFFF;
enum class StopKind { None, Svc, MemoryFault, Exception };
struct Stop {
    StopKind kind = StopKind::None;
    std::uint32_t swi = 0;
    std::uint32_t fault_addr = 0;
    bool fault_write = false;
    Dynarmic::A32::Exception exception{};
    std::uint32_t pc = 0;   // PC after the stop (for Svc: next instruction)
};
class GuestThread final : public Dynarmic::A32::UserCallbacks {
public:
    GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id);
    std::array<std::uint32_t, 16>& regs();
    std::uint32_t cpsr() const;  void set_cpsr(std::uint32_t);
    std::uint32_t tls() const;   void set_tls(std::uint32_t);
    Stop run();                  // Run() until a callback halts; ClearHalt(UserDefined1)
    void invalidate(std::uint32_t addr, std::uint32_t len);
    std::uint64_t sigmask = 0;   // guest signal mask (emulated)
    // UserCallbacks overrides ...
};
}
```
Jit config:
- `arch_version = v8`, `fastmem_pointer = base`, `coprocessors[15] = cp15`
- `define_unpredictable_behaviour = true`, `enable_cycle_counting = false`
- `check_halt_on_memory_access = true`, `code_cache_size = 32 MiB`
- `global_monitor`, `processor_id`

Callbacks:
- `MemoryRead*`/`MemoryWrite*` check `accessible`. On failure they record
  `MemoryFault` (first one wins), `HaltExecution(UserDefined1)`, and return 0.
- `MemoryWriteExclusive*`: compare current == expected, then write; return true.
- `MemoryReadCode` returns `nullopt` unless the page is exec.
- `CallSVC` records `Svc`; `ExceptionRaised`/`InterpreterFallback` record `Exception`.
  All three halt.
- CP15: `MRC p15,0,Rt,c13,c0,3` -> `&tpidruro`; `c13,c0,2` get/set -> `&tpidrurw`;
  everything else `monostate`/`nullopt`.

- [ ] Write `tests/host/t1_blob_test.cpp`:
```cpp
#include "check.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include <dynarmic/interface/exclusive_monitor.h>
#include <cstring>
#include <sys/mman.h>
static void put32(zb::GuestMemory& m, std::uint32_t a, std::uint32_t v) { std::memcpy(m.base() + a, &v, 4); }
int main() {
    zb::GuestMemory mem;
    CHECK(mem.ok());
    Dynarmic::ExclusiveMonitor mon(1);
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    put32(mem, 0x10000, 0xE0800001);  // add r0, r0, r1
    put32(mem, 0x10004, 0xE1A00080);  // mov r0, r0, lsl #1
    put32(mem, 0x10008, 0xEF5AFFFF);  // svc #0x5affff
    put32(mem, 0x1000C, 0xEE1D0F70);  // mrc p15, 0, r0, c13, c0, 3
    put32(mem, 0x10010, 0xEF5AFFFF);  // svc #0x5affff
    put32(mem, 0x10014, 0xE5910000);  // ldr r0, [r1]
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    zb::GuestThread t(mem, &mon, 0);
    t.set_cpsr(0x10);
    t.regs()[0] = 20; t.regs()[1] = 1; t.regs()[15] = 0x10000;
    zb::Stop s = t.run();
    CHECK(s.kind == zb::StopKind::Svc);
    CHECK(s.swi == zb::kHostReturnSwi);
    CHECK(t.regs()[0] == 42);
    CHECK(s.pc == 0x1000C);
    t.set_tls(0x12345678);
    s = t.run();
    CHECK(s.kind == zb::StopKind::Svc);
    CHECK(t.regs()[0] == 0x12345678);
    t.regs()[1] = 0;
    s = t.run();
    CHECK(s.kind == zb::StopKind::MemoryFault);
    CHECK(s.fault_addr == 0);
    CHECK(!s.fault_write);
    std::puts("t1_blob_test PASS");
}
```
- [ ] Run ctest. Expected: FAIL (build).
- [ ] Implement. Run ctest. Expected: `t1_blob_test PASS`. **This is acceptance T1.**
- [ ] Commit `core: GuestThread over Dynarmic A32 with CP15 TLS (T1)`.

### Task 4: syscall tables + guest ABI + NDK ABI check

**Files:** `tools/gen_syscalls.py`, `core/src/gen/syscall_nrs_arm.h`,
`core/src/gen/syscall_names_arm.inc`, `core/include/zb/guest_abi.h`, `tools/abi_check.c`,
`tools/build_guest.sh`.

- **Generator.** Parses every `#define __NR_(\w+) \(__NR_SYSCALL_BASE \+ (\d+)\)` and
  emits `inline constexpr std::uint32_t NR_<name> = N;` in `namespace zb`. It appends
  ARM-private numbers (`NR_ARM_breakpoint=0xF0001`, `NR_ARM_cacheflush=0xF0002`,
  `NR_ARM_usr26=0xF0003`, `NR_ARM_usr32=0xF0004`, `NR_ARM_set_tls=0xF0005`,
  `NR_ARM_get_tls=0xF0006`). The `.inc` holds `{N, "name"},` rows. ASCII only; the NDK
  path defaults to `~/android-ndk-r29`.
- **`guest_abi.h`** (namespace `zb::g`): `timespec32 {i32,i32}`, `timeval32 {i32,i32}`,
  `timespec64 {i64,i64}`, `iovec32 {u32,u32}`, `stack32 {u32 sp; i32 flags; u32 size}`,
  `rlimit32 {u32,u32}`, `ksigaction32 {u32 handler; u32 flags; u32 restorer; u64 mask}`
  (20 bytes, packed), and `stat64`.
  - `stat64` fields: `u64 st_dev; u8 pad0[4]; u32 st_ino_trunc; u32 st_mode; u32 st_nlink;
    u32 st_uid; u32 st_gid; u64 st_rdev; u8 pad3[4]; u32 pad4; i64 st_size;
    u32 st_blksize; u32 pad5; u64 st_blocks; u32 st_atime_sec, st_atime_nsec,
    st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec; u64 st_ino`.
    Size 104; offsets size=48, blksize=56, blocks=64, atime=72, ino=96.
  - Do not name members `st_atime`/`st_mtime`/`st_ctime`: glibc macros.
  - Each layout gets `static_assert`s.
- **`tools/abi_check.c`.** `_Static_assert`s the same sizes and offsets on bionic's
  `struct stat64`, `struct timespec`, `struct iovec` and `stack_t`.
- **`tools/build_guest.sh`** (`set -eu`). Compiles `tools/abi_check.c` with `-c`, and
  `guest/tests/*.c` with `-static -O2` into `build/guest/`.
- [ ] Run `python3 tools/gen_syscalls.py`. Expected: `NR_openat = 322`,
  `NR_exit_group = 248`, `NR_mmap2 = 192`.
- [ ] Run `tools/build_guest.sh` with a placeholder `guest/tests/hello_static.c`
  (`int main(){return 0;}`). Expected: abi_check compiles (all asserts hold).
- [ ] Commit `core: generated arm EABI syscall table, guest ABI layouts checked against bionic`.

### Task 5: ELF loader + initial stack (TDD)

**Files:** `core/include/zb/elf_loader.h`, `core/src/elf_loader.cpp`,
`core/include/zb/initial_stack.h`, `core/src/initial_stack.cpp`,
`tests/host/elf_loader_test.cpp`.

Interface:
```cpp
namespace zb {
struct LoadedElf {
    std::uint32_t bias = 0, entry = 0, phdr = 0, phnum = 0, load_start = 0, load_end = 0;
    bool is_dyn = false;
    std::string interp;
};
bool load_elf(GuestMemory& mem, const std::string& path, std::uint32_t dyn_limit, LoadedElf& out, std::string& error);
struct AuxEntry { std::uint32_t type, value; };
// Kernel-style stack below stack_top. Appends AT_RANDOM, AT_PLATFORM("v8l"), AT_EXECFN, AT_NULL.
// Returns sp (8-byte aligned, argc at sp) or 0 on failure.
std::uint32_t build_initial_stack(GuestMemory& mem, std::uint32_t stack_top, const std::vector<std::string>& argv,
                                  const std::vector<std::string>& envp, std::vector<AuxEntry> auxv, const std::string& execfn);
}
```
**Loader algorithm:**
1. Read the whole file. Validate ELFCLASS32/ELFDATA2LSB/EM_ARM and ET_EXEC or ET_DYN,
   and that the phdr table is in range.
2. Span = [min page_down(p_vaddr), max page_up(p_vaddr + p_memsz)) over PT_LOAD.
3. Placement: ET_EXEC must be `range_free`. ET_DYN goes at `find_free(span, dyn_limit)`,
   so bias = placement - min.
4. Map the span RW, memcpy each segment's file bytes, then `protect` each PT_LOAD per
   `p_flags`.
5. `interp` comes from PT_INTERP. `phdr` comes from PT_PHDR + bias, otherwise from the
   PT_LOAD that contains `e_phoff`.

**Stack layout, high to low:**
- execfn, "v8l", env strings, arg strings;
- 16 random bytes (`getrandom`);
- align to 16;
- table: argc, argv..., 0, envp..., 0, auxv pairs..., AT_NULL;
- final sp `& ~7`.

- [ ] Test: build `guest/tests/hello_static.c` first (Task 4 script). Then
  `elf_loader_test` loads `build/guest/hello_static`, with the path passed via
  `argv[1]` from `add_test(... COMMAND elf_loader_test ${CMAKE_SOURCE_DIR}/build/guest/hello_static)`.
  CHECKs:
  - `!is_dyn`, `interp.empty()`
  - `entry` inside `[load_start, load_end)` and exec-accessible
  - `phnum > 0`, `phdr` readable
  - `build_initial_stack` with argv `{"prog","a"}`, envp `{"X=1"}`, auxv
    `{{AT_PAGESZ,4096}}` returns sp with `(sp & 7) == 0`; `*(u32*)sp == 2`; the string
    at the argv[1] pointer is `"a"`
  - the AT_RANDOM entry is present and points to readable memory
- [ ] Run -> FAIL; implement; run -> PASS.
- [ ] Commit `core: ELF32 loader and initial process stack`.

### Task 6: Process + syscalls + zbrun (T2)

**Files:** `core/include/zb/syscalls.h`, `core/src/syscalls.cpp`, `core/include/zb/process.h`,
`core/src/process.cpp`, `cli/zbrun/main.cpp` (real), `guest/tests/hello_static.c` (real),
`guest/tests/expected/hello_static.out`, `tools/run_guest_tests.sh`.

**`Process`:**
- **Members:** `GuestMemory`, `ExclusiveMonitor(256)`, main `GuestThread`, `brk_start`,
  `brk_current`, `mmap_limit = 0xFE000000`, `clear_child_tid`,
  `std::array<g::ksigaction32, 65> sigactions`, exit state.
- **Constants:** `kStackTop = 0xFF000000`, `kStackSize = 8 MiB`.
- **`run(path, argv, envp)`:**
  1. Load the ELF; a non-empty interp logs "needs interpreter (Phase 2)" and returns 1.
  2. Map the stack. Set brk = load_end.
  3. Build auxv: AT_PHDR, AT_PHENT = 32, AT_PHNUM, AT_PAGESZ, AT_BASE = 0,
     AT_FLAGS = 0, AT_ENTRY, AT_UID/EUID/GID/EGID, AT_HWCAP = HALF|THUMB|FAST_MULT|VFP|
     EDSP|NEON|VFPv3|TLS|VFPv4|IDIVA|IDIVT|VFPD32|LPAE, AT_HWCAP2 = 0, AT_CLKTCK = 100,
     AT_SECURE = 0.
  4. Registers: sp, pc = entry & ~1, cpsr = 0x10 | (entry & 1 ? 0x20 : 0).
  5. Loop:
     - `Svc` with swi 0 -> `handle_syscall`; if it returns false, return the exit status.
     - Other `Svc` -> log and r0 = -ENOSYS.
     - `MemoryFault` -> crash report, return 139.
     - `Exception` -> crash report, return 132.
- **`invalidate(addr, len)`** forwards to every live thread (main only in Phase 1).
- **Crash report:** kind, pc, fault address + R/W, r0-r15, cpsr, and the name of the
  exception.

**`handle_syscall(Process&, GuestThread&)`** reads r7/r0-r5 and writes r0. Unknown numbers
log `unimplemented syscall <name> (<nr>)` once per number and return -ENOSYS.
Pointer args are validated with `host_ptr` (-EFAULT on failure). Guest strings are
validated page by page up to the NUL. Host results `-1` become `-errno`.

| Syscall(s) | Semantics |
|---|---|
| exit, exit_group | request_exit(a0 & 0xff); return false |
| read, write, pread64(a3 pad, off = a4 \| a5<<32), writev, readv | buffers via host_ptr; iovec32 converted |
| openat, close, dup, dup3, pipe2, readlinkat, faccessat, faccessat2, getcwd, unlinkat, mkdirat | pass-through with path/buffer conversion (open flags identical on arm/arm64) |
| lseek | host lseek; result > INT32_MAX -> -EOVERFLOW |
| _llseek | fd, hi, lo, result*, whence; writes i64 |
| fstat64, fstatat64 | host fstat/fstatat, fill g::stat64 |
| statx | pass-through (layout is fixed-width) |
| fcntl64 | F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL, F_DUPFD_CLOEXEC pass-through; others -EINVAL (logged once) |
| ioctl | allowlist TCGETS(36 bytes), TIOCGWINSZ(8), FIONBIO(4), FIONREAD(4); others -ENOTTY (logged once) |
| brk | grow: map pages between old and new top if range_free and below mmap_limit; shrink: unmap; failure returns current brk |
| mmap2 | MAP_FIXED / MAP_FIXED_NOREPLACE honored; hint used if free and >= 0x10000; else find_free(len, mmap_limit); anon -> map_anon, file -> map_file(offset = pgoff*4096); invalidate |
| munmap, mprotect | page-aligned addr required; unmapped range -> -ENOMEM for mprotect; invalidate |
| madvise | host madvise on the mapped range (keeps MADV_DONTNEED zeroing semantics) |
| mremap | -ENOMEM |
| ARM_set_tls / ARM_get_tls / ARM_cacheflush | set thread TLS / return TLS / invalidate [a0,a1) |
| set_tid_address | store clear_child_tid; return host gettid |
| getpid, getppid, gettid, getuid32, geteuid32, getgid32, getegid32 | host syscall |
| rt_sigaction | sig 1..64, sigsetsize 8; copy old/new ksigaction32 in the Process table |
| rt_sigprocmask | SIG_BLOCK/UNBLOCK/SETMASK on thread sigmask; write old mask (8 bytes) |
| sigaltstack | stored per thread; old defaults to SS_DISABLE |
| kill, tkill, tgkill | target self with a signal whose disposition is SIG_DFL (or SIGKILL): log, request_exit(128+sig), return false; ignored signal: 0; else -EPERM |
| clock_gettime, clock_getres, gettimeofday | timespec32/timeval32 conversion |
| clock_gettime64, clock_getres_time64 | timespec64 pass-through |
| nanosleep, clock_nanosleep, clock_nanosleep_time64 | convert request/remain |
| futex, futex_time64 | WAIT/WAKE (+PRIVATE/CLOCK_REALTIME, +BITSET); timeout converted (32-bit variant); others -ENOSYS |
| sched_yield, sched_getaffinity | pass-through |
| getrandom | pass-through |
| prctl | PR_SET_VMA -> 0; PR_SET_NAME/PR_GET_NAME with 16-byte buffer; others -EINVAL (logged once) |
| uname | host uname then machine = "armv8l" |
| ugetrlimit | host getrlimit, clamp to rlimit32 (RLIM_INFINITY -> 0xFFFFFFFF) |
| prlimit64 | pass-through (fixed-width) |
| personality | emulated per process: query (0xFFFFFFFF) returns current, set returns previous. Found during execution: bionic arm32 aborts if PER_LINUX32 cannot be set, and this kernel refuses it |
| sched_getscheduler, socket, connect | pass-through; found during execution (bionic init, liblog's logd socket) |
| rt_tgsigqueueinfo | same as tgkill; found during execution (bionic abort uses it) |

- [ ] Real `guest/tests/hello_static.c`:
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char** argv) {
    printf("hello arm32\n");
    printf("argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "(none)");
    char* big = malloc(1 << 20);
    memset(big, 0x5a, 1 << 20);
    printf("malloc=%s\n", big[12345] == 0x5a ? "PASS" : "FAIL");
    free(big);
    void* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("mmap=%s\n", p != MAP_FAILED ? "PASS" : "FAIL");
    ((char*)p)[65535] = 1;
    munmap(p, 65536);
    struct timespec ts;
    printf("clock=%s\n", clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? "PASS" : "FAIL");
    printf("pid=%s\n", getpid() > 0 ? "PASS" : "FAIL");
    return 7;
}
```
Expected `guest/tests/expected/hello_static.out`:
```
hello arm32
argc=2 argv1=world
malloc=PASS
mmap=PASS
clock=PASS
pid=PASS
```
- [ ] `tools/run_guest_tests.sh`: `run_case hello_static 7 world` runs
  `build/host/cli/zbrun/zbrun build/guest/hello_static world`. It compares the exit code
  and diffs stdout with the expected file, printing `PASS <name>` or `FAIL <name>: ...`.
  It exits non-zero on any failure and saves stderr to `build/guest/<name>.stderr`.
- [ ] Iterate: build, run, read `unimplemented syscall` / crash-report lines in stderr,
  implement per the table, repeat until PASS. Anything not in the table gets a row
  added to this plan and to the spec's syscall section when implemented.
- [ ] **Acceptance T2:** `tools/run_guest_tests.sh` prints `PASS hello_static`; ctest all pass.
- [ ] Update CLAUDE.md "Current state" + build/test commands (the two code blocks above).
- [ ] Commit `zbrun: static arm32 executables run through Dynarmic (T2)`.
