#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "zb/guest_abi.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace zb {

// One guest process: its address space, emulated kernel state and threads.
class Process {
public:
    static constexpr std::uint32_t kStackTop = 0xFF000000;
    static constexpr std::uint32_t kStackSize = 8 * 1024 * 1024;
    static constexpr std::uint32_t kMmapLimit = 0xFE000000;

    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Loads an arm32 executable, builds its stack and runs it to completion.
    // Returns the guest exit status, or 128 + signal for a fatal guest fault.
    int run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp);

    GuestMemory& memory() { return mem_; }
    void request_exit(int status);
    int exit_status() const { return exit_status_; }

    // Drops translated code for the range in every thread.
    void invalidate(std::uint32_t addr, std::uint32_t len);
    // True the first time `key` is seen; used to log each unsupported case once.
    bool first_time(std::uint64_t key);

    std::uint32_t brk_start = 0;
    std::uint32_t brk_current = 0;
    std::uint32_t mmap_limit = kMmapLimit;
    std::uint32_t clear_child_tid = 0;
    // Emulated: a 64-bit-only host kernel refuses PER_LINUX32, and bionic aborts if that fails.
    std::uint32_t personality = 0;
    std::array<g::ksigaction32, 65> sigactions{};
    // Single-threaded until guest threads exist; moves to GuestThread then.
    g::stack32 altstack{0, 2 /* SS_DISABLE */, 0};

private:
    void crash_report(const Stop& stop, GuestThread& thread) const;

    GuestMemory mem_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor_;
    std::unique_ptr<GuestThread> main_;
    std::set<std::uint64_t> seen_;
    int exit_status_ = 0;
};

}  // namespace zb
