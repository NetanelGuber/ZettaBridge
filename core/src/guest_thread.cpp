#include "zb/guest_thread.h"

#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/halt_reason.h>

#include "zb/cp15.h"
#include "zb/log.h"

namespace zb {

namespace {

constexpr Dynarmic::HaltReason kStopHalt = Dynarmic::HaltReason::UserDefined1;

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
void store(std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

}  // namespace

GuestThread::GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id) : mem_(mem) {
    cp15_ = std::make_shared<Cp15>(&tpidruro_, &tpidrurw_);

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.global_monitor = monitor;
    cfg.processor_id = processor_id;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
    cfg.fastmem_pointer = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(mem.base()));
    cfg.coprocessors[15] = cp15_;
    cfg.define_unpredictable_behaviour = true;
    cfg.enable_cycle_counting = false;
    cfg.check_halt_on_memory_access = true;
    cfg.code_cache_size = 32 * 1024 * 1024;
    jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

GuestThread::~GuestThread() = default;

std::array<std::uint32_t, 16>& GuestThread::regs() {
    return jit_->Regs();
}

std::uint32_t GuestThread::cpsr() const {
    return jit_->Cpsr();
}

void GuestThread::set_cpsr(std::uint32_t value) {
    jit_->SetCpsr(value);
}

Stop GuestThread::run() {
    pending_ = Stop{};
    jit_->Run();
    jit_->ClearHalt(kStopHalt);
    Stop s = pending_;
    if (s.kind != StopKind::Exception) s.pc = jit_->Regs()[15];
    return s;
}

void GuestThread::invalidate(std::uint32_t addr, std::uint32_t len) {
    jit_->InvalidateCacheRange(addr, len);
}

void GuestThread::halt() {
    jit_->HaltExecution(kStopHalt);
}

bool GuestThread::check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write) {
    if (mem_.accessible(vaddr, len, need)) return true;
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::MemoryFault;
        pending_.fault_addr = vaddr;
        pending_.fault_write = write;
    }
    halt();
    return false;
}

std::uint8_t GuestThread::MemoryRead8(std::uint32_t vaddr) {
    return check_access(vaddr, 1, kPageRead, false) ? mem_.base()[vaddr] : 0;
}

std::uint16_t GuestThread::MemoryRead16(std::uint32_t vaddr) {
    return check_access(vaddr, 2, kPageRead, false) ? load<std::uint16_t>(mem_.base() + vaddr) : 0;
}

std::uint32_t GuestThread::MemoryRead32(std::uint32_t vaddr) {
    return check_access(vaddr, 4, kPageRead, false) ? load<std::uint32_t>(mem_.base() + vaddr) : 0;
}

std::uint64_t GuestThread::MemoryRead64(std::uint32_t vaddr) {
    return check_access(vaddr, 8, kPageRead, false) ? load<std::uint64_t>(mem_.base() + vaddr) : 0;
}

void GuestThread::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (check_access(vaddr, 1, kPageWrite, true)) mem_.base()[vaddr] = value;
}

void GuestThread::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (check_access(vaddr, 2, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (check_access(vaddr, 4, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (check_access(vaddr, 8, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

// The ExclusiveMonitor serializes these calls; compare-then-write is atomic under its lock.
bool GuestThread::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    if (!check_access(vaddr, 1, kPageRead | kPageWrite, true) || mem_.base()[vaddr] != expected) return false;
    mem_.base()[vaddr] = value;
    return true;
}

bool GuestThread::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    if (!check_access(vaddr, 2, kPageRead | kPageWrite, true) || load<std::uint16_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

bool GuestThread::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    if (!check_access(vaddr, 4, kPageRead | kPageWrite, true) || load<std::uint32_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

bool GuestThread::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    if (!check_access(vaddr, 8, kPageRead | kPageWrite, true) || load<std::uint64_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

std::optional<std::uint32_t> GuestThread::MemoryReadCode(std::uint32_t vaddr) {
    if (!mem_.accessible(vaddr, 4, kPageExec)) return std::nullopt;
    return load<std::uint32_t>(mem_.base() + vaddr);
}

void GuestThread::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    log("interpreter fallback requested at pc 0x%08x (%zu instructions)", pc, num_instructions);
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = Dynarmic::A32::Exception::UndefinedInstruction;
        pending_.pc = pc;
    }
    halt();
}

void GuestThread::CallSVC(std::uint32_t swi) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Svc;
        pending_.swi = swi;
    }
    halt();
}

void GuestThread::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = exception;
        pending_.pc = pc;
    }
    halt();
}

}  // namespace zb
