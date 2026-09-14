#include "zb/process.h"

#include <elf.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string_view>
#include <thread>

#include <dynarmic/interface/exclusive_monitor.h>

#include "zb/elf_loader.h"
#include "zb/initial_stack.h"
#include "zb/log.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// arch/arm/include/uapi/asm/hwcap.h
constexpr std::uint32_t kHwcapHalf = 1u << 1;
constexpr std::uint32_t kHwcapThumb = 1u << 2;
constexpr std::uint32_t kHwcapFastMult = 1u << 4;
constexpr std::uint32_t kHwcapVfp = 1u << 6;
constexpr std::uint32_t kHwcapEdsp = 1u << 7;
constexpr std::uint32_t kHwcapNeon = 1u << 12;
constexpr std::uint32_t kHwcapVfpv3 = 1u << 13;
constexpr std::uint32_t kHwcapTls = 1u << 15;
constexpr std::uint32_t kHwcapVfpv4 = 1u << 16;
constexpr std::uint32_t kHwcapIdiva = 1u << 17;
constexpr std::uint32_t kHwcapIdivt = 1u << 18;
constexpr std::uint32_t kHwcapVfpd32 = 1u << 19;
constexpr std::uint32_t kHwcapLpae = 1u << 20;
constexpr std::uint32_t kGuestHwcap = kHwcapHalf | kHwcapThumb | kHwcapFastMult | kHwcapVfp | kHwcapEdsp | kHwcapNeon |
                                      kHwcapVfpv3 | kHwcapTls | kHwcapVfpv4 | kHwcapIdiva | kHwcapIdivt |
                                      kHwcapVfpd32 | kHwcapLpae;

constexpr std::uint32_t kCpsrUserMode = 0x10;
constexpr std::uint32_t kCpsrThumb = 0x20;

// Linux clone flags (identical on arm and arm64).
constexpr std::uint32_t kCloneSetTls = 0x00080000;
constexpr std::uint32_t kCloneParentSetTid = 0x00100000;
constexpr std::uint32_t kCloneChildClearTid = 0x00200000;
constexpr std::uint32_t kCloneChildSetTid = 0x01000000;

constexpr std::uint64_t kSeenUnexpectedSvc = 1ULL << 40;
constexpr std::uint64_t kSeenHostCall = 1ULL << 41;

// svc #(0x5A0000 | index) from the generated stub libraries (tools/gen_stubs.py).
constexpr std::uint32_t kHostCallBase = 0x5A0000;

struct HostCallName {
    std::uint32_t index;
    const char* library;
    const char* name;
};

constexpr HostCallName kHostCallNames[] = {
#include "gen/hostcalls.inc"
};

struct PathMapping {
    std::string_view guest_prefix;
    std::string_view sysroot_prefix;
};

// The runtime APEX paths collapse onto the bootstrap copies extracted into system/.
constexpr PathMapping kPathMappings[] = {
    {"/apex/com.android.runtime/lib/bionic/", "/system/lib/"},
    {"/apex/com.android.runtime/bin/", "/system/bin/"},
    {"/system/", "/system/"},
    {"/vendor/", "/vendor/"},
    {"/odm/", "/odm/"},
    {"/product/", "/product/"},
    {"/system_ext/", "/system_ext/"},
    {"/apex/", "/apex/"},
    {"/linkerconfig/", "/linkerconfig/"},
};

const char* exception_name(Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::UndefinedInstruction: return "undefined instruction";
    case E::UnpredictableInstruction: return "unpredictable instruction";
    case E::DecodeError: return "decode error";
    case E::SendEvent: return "SEV";
    case E::SendEventLocal: return "SEVL";
    case E::WaitForInterrupt: return "WFI";
    case E::WaitForEvent: return "WFE";
    case E::Yield: return "YIELD";
    case E::Breakpoint: return "breakpoint";
    case E::PreloadData: return "PLD";
    case E::PreloadDataWithIntentToWrite: return "PLDW";
    case E::PreloadInstruction: return "PLI";
    case E::NoExecuteFault: return "jump to non-executable memory";
    }
    return "unknown exception";
}

void write_guest_u32(GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    if (std::uint8_t* p = mem.host_ptr(addr, 4, kPageWrite)) std::memcpy(p, &value, 4);
}

}  // namespace

Process::Process() : monitor_(std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxThreads)) {}

Process::~Process() = default;

void Process::request_exit(int status) {
    exit_status_ = status;
    exiting_ = true;
}

void Process::invalidate(std::uint32_t addr, std::uint32_t len) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : threads_) t->invalidate(addr, len);
}

bool Process::first_time(std::uint64_t key) {
    std::lock_guard<std::mutex> lock(seen_mutex_);
    return seen_.insert(key).second;
}

void Process::record_file_mapping(std::uint32_t start, std::uint32_t length, std::uint64_t offset, std::string path,
                                  bool offset_is_vaddr) {
    forget_mappings(start, length);
    file_mappings_.push_back({start, length, offset, std::move(path), offset_is_vaddr});
}

void Process::forget_mappings(std::uint32_t start, std::uint64_t length) {
    const std::uint64_t end = static_cast<std::uint64_t>(start) + length;
    std::erase_if(file_mappings_, [&](const FileMapping& m) {
        return m.start < end && static_cast<std::uint64_t>(m.start) + m.length > start;
    });
    std::erase_if(textrel_ranges_, [&](const std::pair<std::uint32_t, std::uint32_t>& r) {
        return r.first < end && static_cast<std::uint64_t>(r.first) + r.second > start;
    });
}

std::string Process::describe_address(std::uint32_t addr) const {
    for (const auto& m : file_mappings_) {
        if (addr >= m.start && addr - m.start < m.length) {
            char where[64];
            std::snprintf(where, sizeof where, " %s 0x%llx", m.offset_is_vaddr ? "vaddr" : "offset",
                          static_cast<unsigned long long>(m.offset + (addr - m.start)));
            return m.path + where;
        }
    }
    return "?";
}

void Process::add_textrel_range(std::uint32_t start, std::uint32_t length) {
    textrel_ranges_.emplace_back(start, length);
}

bool Process::overlaps_textrel_range(std::uint32_t start, std::uint64_t length) const {
    const std::uint64_t end = static_cast<std::uint64_t>(start) + length;
    for (const auto& [range_start, range_length] : textrel_ranges_) {
        if (range_start < end && static_cast<std::uint64_t>(range_start) + range_length > start) return true;
    }
    return false;
}

std::string Process::translate_path(const char* guest_path) const {
    const std::string_view path(guest_path);
    if (path == "/proc/self/exe") return exe_path_;
    if (!sysroot_.empty()) {
        for (const auto& m : kPathMappings) {
            if (path.substr(0, m.guest_prefix.size()) == m.guest_prefix) {
                std::string out = sysroot_;
                out += m.sysroot_prefix;
                out += path.substr(m.guest_prefix.size());
                return out;
            }
        }
    }
    return std::string(path);
}

std::size_t Process::thread_count() const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return threads_.size();
}

int Process::allocate_processor_id() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (std::size_t i = 0; i < processor_ids_.size(); ++i) {
        if (!processor_ids_.test(i)) {
            processor_ids_.set(i);
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Process::register_thread(GuestThread* thread) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    threads_.push_back(thread);
}

int Process::run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp) {
    if (!mem_.ok()) return 1;

    char resolved[PATH_MAX];
    exe_path_ = realpath(path.c_str(), resolved) != nullptr ? resolved : path;

    LoadedElf exe;
    std::string error;
    if (!load_elf(mem_, path, kExecutableLimit, exe, error)) {
        log("%s", error.c_str());
        return 1;
    }
    record_file_mapping(exe.load_start, exe.load_end - exe.load_start, exe.load_start - exe.bias, exe_path_, true);

    std::uint32_t start_pc = exe.entry;
    std::uint32_t interp_base = 0;
    if (!exe.interp.empty()) {
        const std::string interp_path = translate_path(exe.interp.c_str());
        LoadedElf interp;
        if (!load_elf(mem_, interp_path, kMmapLimit, interp, error)) {
            log("cannot load the dynamic linker %s: %s", exe.interp.c_str(), error.c_str());
            if (sysroot_.empty()) log("pass --sysroot or set ZB_SYSROOT");
            return 1;
        }
        record_file_mapping(interp.load_start, interp.load_end - interp.load_start, interp.load_start - interp.bias,
                            interp_path, true);
        interp_base = interp.bias;
        start_pc = interp.entry;
    }

    if (!mem_.map_anon(kStackTop - kStackSize, kStackSize, PROT_READ | PROT_WRITE)) {
        log("cannot map the guest stack");
        return 1;
    }
    brk_start = brk_current = exe.load_end;

    const std::vector<AuxEntry> auxv = {
        {AT_PHDR, exe.phdr},
        {AT_PHENT, sizeof(Elf32_Phdr)},
        {AT_PHNUM, exe.phnum},
        {AT_PAGESZ, kPageSize},
        {AT_BASE, interp_base},
        {AT_FLAGS, 0},
        {AT_ENTRY, exe.entry},
        {AT_UID, static_cast<std::uint32_t>(getuid())},
        {AT_EUID, static_cast<std::uint32_t>(geteuid())},
        {AT_GID, static_cast<std::uint32_t>(getgid())},
        {AT_EGID, static_cast<std::uint32_t>(getegid())},
        {AT_HWCAP, kGuestHwcap},
        {AT_HWCAP2, 0},
        {AT_CLKTCK, 100},
        {AT_SECURE, 0},
    };
    const std::uint32_t sp = build_initial_stack(mem_, kStackTop, argv, envp, auxv, exe_path_);
    if (sp == 0) {
        log("arguments and environment do not fit on the guest stack");
        return 1;
    }

    main_ = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(allocate_processor_id()));
    auto& regs = main_->regs();
    regs.fill(0);
    regs[13] = sp;
    regs[15] = start_pc & ~1u;
    main_->set_cpsr(kCpsrUserMode | ((start_pc & 1) ? kCpsrThumb : 0));
    register_thread(main_.get());

    thread_loop(*main_);
    if (exiting_) return exit_status_;

    // The main thread called exit() while other threads may still run: wait for them.
    const int main_status = main_->exit_status;
    finish_thread(*main_);
    wait_for_threads();
    return exiting_ ? exit_status_.load() : main_status;
}

void Process::thread_loop(GuestThread& thread) {
    for (;;) {
        const Stop stop = thread.run();
        switch (stop.kind) {
        case StopKind::Svc:
            if (stop.swi == 0) {
                if (handle_syscall(*this, thread)) break;
                if (exiting_ && thread_count() > 1) exit_host_process();
                return;
            }
            if ((stop.swi & 0xFF0000u) == kHostCallBase && stop.swi != kHostReturnSwi) {
                const std::uint32_t index = stop.swi & 0xFFFFu;
                if (first_time(kSeenHostCall | index)) {
                    const char* library = "?";
                    const char* name = "?";
                    for (const auto& h : kHostCallNames) {
                        if (h.index == index) {
                            library = h.library;
                            name = h.name;
                            break;
                        }
                    }
                    log("host call %s:%s is not implemented yet", library, name);
                }
                thread.regs()[0] = 0;
                break;
            }
            if (first_time(kSeenUnexpectedSvc | stop.swi)) log("unexpected svc #0x%x at pc 0x%08x", stop.swi, stop.pc);
            thread.regs()[0] = static_cast<std::uint32_t>(-ENOSYS);
            break;
        case StopKind::MemoryFault:
            crash_report(stop, thread);
            request_exit(128 + SIGSEGV);
            if (thread_count() > 1) exit_host_process();
            return;
        case StopKind::Exception:
            crash_report(stop, thread);
            request_exit(128 + SIGILL);
            if (thread_count() > 1) exit_host_process();
            return;
        case StopKind::None:
            log("guest stopped without a reason at pc 0x%08x", stop.pc);
            request_exit(1);
            if (thread_count() > 1) exit_host_process();
            return;
        }
    }
}

std::int32_t Process::clone_thread(GuestThread& parent, std::uint32_t flags, std::uint32_t stack,
                                   std::uint32_t parent_tid_addr, std::uint32_t tls, std::uint32_t child_tid_addr) {
    const int processor_id = allocate_processor_id();
    if (processor_id < 0) return -EAGAIN;

    auto child = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id));
    child->regs() = parent.regs();
    child->regs()[0] = 0;
    if (stack != 0) child->regs()[13] = stack;
    child->ext_regs() = parent.ext_regs();
    child->set_cpsr(parent.cpsr());
    child->set_fpscr(parent.fpscr());
    child->set_tls((flags & kCloneSetTls) ? tls : parent.tls());
    child->sigmask = parent.sigmask;
    if (flags & kCloneChildClearTid) child->clear_child_tid = child_tid_addr;

    std::promise<pid_t> tid_promise;
    std::future<pid_t> tid_future = tid_promise.get_future();
    register_thread(child.get());
    std::thread([this, owned = std::move(child), promise = &tid_promise, flags, child_tid_addr]() mutable {
        const auto tid = static_cast<pid_t>(::syscall(SYS_gettid));
        if (flags & kCloneChildSetTid) write_guest_u32(mem_, child_tid_addr, static_cast<std::uint32_t>(tid));
        promise->set_value(tid);
        thread_main(std::move(owned));
    }).detach();

    const pid_t tid = tid_future.get();
    if (flags & kCloneParentSetTid) write_guest_u32(mem_, parent_tid_addr, static_cast<std::uint32_t>(tid));
    return tid;
}

void Process::thread_main(std::unique_ptr<GuestThread> thread) {
    thread_loop(*thread);
    finish_thread(*thread);
}

void Process::finish_thread(GuestThread& thread) {
    if (thread.clear_child_tid != 0) {
        if (std::uint8_t* p = mem_.host_ptr(thread.clear_child_tid, 4, kPageWrite)) {
            const std::uint32_t zero = 0;
            std::memcpy(p, &zero, 4);
            ::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }
    }
    monitor_->ClearProcessor(thread.processor_id());
    std::lock_guard<std::mutex> lock(threads_mutex_);
    std::erase(threads_, &thread);
    processor_ids_.reset(thread.processor_id());
    threads_cv_.notify_all();
}

void Process::wait_for_threads() {
    std::unique_lock<std::mutex> lock(threads_mutex_);
    threads_cv_.wait(lock, [&] { return threads_.empty(); });
}

void Process::exit_host_process() {
    std::fflush(stderr);
    std::_Exit(exit_status_);
}

void Process::crash_report(const Stop& stop, GuestThread& thread) const {
    if (stop.kind == StopKind::MemoryFault) {
        log("guest SIGSEGV: %s of 0x%08x, pc 0x%08x", stop.fault_write ? "write" : "read", stop.fault_addr, stop.pc);
    } else {
        log("guest SIGILL: %s at pc 0x%08x", exception_name(stop.exception), stop.pc);
    }
    const auto& r = thread.regs();
    for (int i = 0; i < 16; i += 4) {
        log("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    }
    log("  cpsr %08x  tls %08x  tid %ld", thread.cpsr(), thread.tls(), static_cast<long>(::syscall(SYS_gettid)));
    log("  pc in %s", describe_address(stop.pc).c_str());
    log("  lr in %s", describe_address(r[14]).c_str());
}

}  // namespace zb
