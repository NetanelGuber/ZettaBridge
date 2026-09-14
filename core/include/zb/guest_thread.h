#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "zb/guest_abi.h"
#include "zb/guest_memory.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace zb {

class Cp15;

// svc immediate used to return from a host->guest call.
inline constexpr std::uint32_t kHostReturnSwi = 0x5AFFFF;

enum class StopKind { None, Svc, MemoryFault, Exception };

struct Stop {
    StopKind kind = StopKind::None;
    std::uint32_t swi = 0;
    std::uint32_t fault_addr = 0;
    bool fault_write = false;
    Dynarmic::A32::Exception exception{};
    // PC after the stop. For Svc it is the instruction after the svc.
    std::uint32_t pc = 0;
};

// One guest CPU context on one host thread. Dynarmic callbacks never do host work: they
// record a Stop and halt the JIT; the caller of run() handles it and calls run() again.
class GuestThread final : public Dynarmic::A32::UserCallbacks {
public:
    GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id);
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

    Stop run();
    // Drop translated code for [addr, addr + len). Safe to call from other host threads.
    void invalidate(std::uint32_t addr, std::uint32_t len);

    // Emulated per-thread kernel state.
    std::uint64_t sigmask = 0;
    g::stack32 altstack{0, 2 /* SS_DISABLE */, 0};
    std::uint32_t clear_child_tid = 0;
    int exit_status = 0;

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;
    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }

private:
    bool check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write);
    void halt();

    GuestMemory& mem_;
    std::size_t processor_id_;
    std::uint32_t tpidruro_ = 0;
    std::uint32_t tpidrurw_ = 0;
    std::shared_ptr<Cp15> cp15_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    Stop pending_;
};

}  // namespace zb
