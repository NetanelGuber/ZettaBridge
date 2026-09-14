#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
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
    // A PIE main executable is placed below this address.
    static constexpr std::uint32_t kExecutableLimit = 0x40000000;
    static constexpr std::size_t kMaxThreads = 256;

    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Host directory holding the arm32 Android system files (system/bin/linker, system/lib/...).
    void set_sysroot(std::string dir) { sysroot_ = std::move(dir); }

    // Loads an arm32 executable (and its PT_INTERP), builds its stack and runs it until the
    // process exits. Returns the guest exit status, or 128 + signal for a fatal guest fault.
    int run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp);

    GuestMemory& memory() { return mem_; }
    // Process-wide exit (exit_group, fatal signal). Threads other than the caller are not
    // stopped here; callers with other live threads end the host process instead.
    void request_exit(int status);
    bool exiting() const { return exiting_; }
    int exit_status() const { return exit_status_; }

    // Starts a guest thread for clone(CLONE_VM | CLONE_THREAD ...). Returns the new tid or -errno.
    std::int32_t clone_thread(GuestThread& parent, std::uint32_t flags, std::uint32_t stack,
                              std::uint32_t parent_tid_addr, std::uint32_t tls, std::uint32_t child_tid_addr);
    std::size_t thread_count() const;
    GuestThread* find_thread(std::int32_t tid);

    // Signals (signals.cpp).
    static void set_current_thread(GuestThread* thread);
    static void install_host_signal_forwarding();
    // Delivers pending, unblocked signals of the thread; false if one terminated the process.
    bool dispatch_pending_signals(GuestThread& thread);
    // Turns a memory fault or exception into a guest signal; false if the guest cannot handle it.
    bool deliver_fault(GuestThread& thread, const Stop& stop);
    // Builds the signal frame and redirects the thread to the handler. True if the signal was
    // handled or ignored; false if its action terminates the process.
    bool deliver_signal(GuestThread& thread, const g::siginfo32& info, bool forced);
    // rt_sigreturn / sigreturn: restores the context saved by deliver_signal.
    bool sigreturn(GuestThread& thread, bool rt);

    // Serializes guest address-space changes (mmap/munmap/mprotect/brk/madvise).
    std::mutex& mm_mutex() { return mm_mutex_; }
    // Serializes the guest signal disposition table.
    std::mutex& signal_mutex() { return signal_mutex_; }

    // Maps absolute guest paths of the Android system (/system, /apex, /vendor, ...) into the
    // sysroot, and /proc/self/exe to the guest executable. Other paths are returned unchanged.
    std::string translate_path(const char* guest_path) const;
    // Host path of the guest executable, as reported by /proc/self/exe.
    const std::string& exe_path() const { return exe_path_; }
    // True if addr lies in the image of the guest dynamic linker.
    bool in_guest_linker(std::uint32_t addr) const { return addr >= linker_start_ && addr < linker_end_; }

    // Drops translated code for the range in every thread.
    void invalidate(std::uint32_t addr, std::uint32_t len);
    // True the first time `key` is seen; used to log each unsupported case once.
    bool first_time(std::uint64_t key);

    // File-backed guest ranges, used to name addresses in crash reports. `offset` is the file
    // offset at `start` (or the ELF virtual address for images placed by our own loader).
    // Callers changing mappings hold mm_mutex().
    void record_file_mapping(std::uint32_t start, std::uint32_t length, std::uint64_t offset, std::string path,
                             bool offset_is_vaddr = false);
    void forget_mappings(std::uint32_t start, std::uint64_t length);
    // "libc.so offset 0x1234" style description, or "?" if the address is not file-backed.
    std::string describe_address(std::uint32_t addr) const;

    // Executable segments of libraries marked DT_ZB_TEXTREL (see elf_fixups.h). They stay
    // writable inside the emulator so text relocations can be applied. forget_mappings drops them.
    void add_textrel_range(std::uint32_t start, std::uint32_t length);
    bool overlaps_textrel_range(std::uint32_t start, std::uint64_t length) const;

    std::uint32_t brk_start = 0;
    std::uint32_t brk_current = 0;
    std::uint32_t mmap_limit = kMmapLimit;
    // Emulated: a 64-bit-only host kernel refuses PER_LINUX32, and bionic aborts if that fails.
    std::uint32_t personality = 0;
    std::array<g::ksigaction32, 65> sigactions{};

private:
    struct FileMapping {
        std::uint32_t start;
        std::uint32_t length;
        std::uint64_t offset;
        std::string path;
        bool offset_is_vaddr;
    };

    int allocate_processor_id();
    void register_thread(GuestThread* thread);
    // Runs a guest thread until it exits. Process-wide exits with other live threads end the
    // host process from here.
    void thread_loop(GuestThread& thread);
    void thread_main(std::unique_ptr<GuestThread> thread);
    // Thread exit bookkeeping: CLONE_CHILD_CLEARTID, exclusive monitor, registry.
    void finish_thread(GuestThread& thread);
    void wait_for_threads();
    [[noreturn]] void exit_host_process();
    void crash_report(const Stop& stop, GuestThread& thread) const;

    GuestMemory mem_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor_;
    std::unique_ptr<GuestThread> main_;

    mutable std::mutex threads_mutex_;
    std::condition_variable threads_cv_;
    std::vector<GuestThread*> threads_;
    std::bitset<kMaxThreads> processor_ids_;

    std::mutex mm_mutex_;
    std::mutex signal_mutex_;
    std::mutex seen_mutex_;
    std::set<std::uint64_t> seen_;
    std::vector<FileMapping> file_mappings_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> textrel_ranges_;
    std::string sysroot_;
    std::string exe_path_;
    std::uint32_t linker_start_ = 0;
    std::uint32_t linker_end_ = 0;
    std::atomic<bool> exiting_{false};
    std::atomic<int> exit_status_{0};
};

}  // namespace zb
