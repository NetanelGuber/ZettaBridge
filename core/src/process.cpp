#include "zb/process.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

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

constexpr std::uint64_t kSeenUnexpectedSvc = 1ULL << 40;

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

}  // namespace

Process::Process() : monitor_(std::make_unique<Dynarmic::ExclusiveMonitor>(256)) {}

Process::~Process() = default;

void Process::request_exit(int status) {
    exit_status_ = status;
}

void Process::invalidate(std::uint32_t addr, std::uint32_t len) {
    if (main_) main_->invalidate(addr, len);
}

bool Process::first_time(std::uint64_t key) {
    return seen_.insert(key).second;
}

int Process::run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp) {
    if (!mem_.ok()) return 1;

    LoadedElf exe;
    std::string error;
    if (!load_elf(mem_, path, kMmapLimit, exe, error)) {
        log("%s", error.c_str());
        return 1;
    }
    if (!exe.interp.empty()) {
        log("%s needs the dynamic linker %s, which is not supported yet", path.c_str(), exe.interp.c_str());
        return 1;
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
        {AT_BASE, 0},
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
    const std::uint32_t sp = build_initial_stack(mem_, kStackTop, argv, envp, auxv, path);
    if (sp == 0) {
        log("arguments and environment do not fit on the guest stack");
        return 1;
    }

    main_ = std::make_unique<GuestThread>(mem_, monitor_.get(), 0);
    auto& regs = main_->regs();
    regs.fill(0);
    regs[13] = sp;
    regs[15] = exe.entry & ~1u;
    main_->set_cpsr(kCpsrUserMode | ((exe.entry & 1) ? kCpsrThumb : 0));

    for (;;) {
        const Stop stop = main_->run();
        switch (stop.kind) {
        case StopKind::Svc:
            if (stop.swi == 0) {
                if (!handle_syscall(*this, *main_)) return exit_status_;
            } else {
                if (first_time(kSeenUnexpectedSvc | stop.swi)) {
                    log("unexpected svc #0x%x at pc 0x%08x", stop.swi, stop.pc);
                }
                main_->regs()[0] = static_cast<std::uint32_t>(-ENOSYS);
            }
            break;
        case StopKind::MemoryFault:
            crash_report(stop, *main_);
            return 128 + SIGSEGV;
        case StopKind::Exception:
            crash_report(stop, *main_);
            return 128 + SIGILL;
        case StopKind::None:
            log("guest stopped without a reason at pc 0x%08x", stop.pc);
            return 1;
        }
    }
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
    log("  cpsr %08x  tls %08x", thread.cpsr(), thread.tls());
}

}  // namespace zb
