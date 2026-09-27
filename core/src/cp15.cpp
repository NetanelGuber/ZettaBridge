#include "zb/cp15.h"

#include <ctime>

namespace zb {

using Dynarmic::A32::CoprocReg;

namespace {

bool is_thread_id_reg(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm) {
    return !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0;
}

// Expose a coherent virtual generic timer: CNTVCT counts nanoseconds from
// CLOCK_MONOTONIC and CNTFRQ describes those units to the ARM32 guest.
std::uint32_t virtual_counter_frequency = 1000000000u;

std::uint64_t read_virtual_counter(void*, std::uint32_t, std::uint32_t) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * virtual_counter_frequency +
           static_cast<std::uint64_t>(now.tv_nsec);
}

}  // namespace

std::optional<Cp15::Callback> Cp15::CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) {
    return std::nullopt;
}

Cp15::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (is_thread_id_reg(two, opc1, CRn, CRm) && opc2 == 2) return tpidrurw_;
    return std::monostate{};
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileSendTwoWords(bool, unsigned, CoprocReg) {
    return std::monostate{};
}

Cp15::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (!two && opc1 == 0 && CRn == CoprocReg::C14 && CRm == CoprocReg::C0 && opc2 == 0)
        return &virtual_counter_frequency;  // CNTFRQ
    if (is_thread_id_reg(two, opc1, CRn, CRm)) {
        if (opc2 == 3) return tpidruro_;
        if (opc2 == 2) return tpidrurw_;
    }
    return std::monostate{};
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    if (!two && opc == 1 && CRm == CoprocReg::C14)
        return Callback{read_virtual_counter, std::nullopt};  // CNTVCT
    return std::monostate{};
}

std::optional<Cp15::Callback> Cp15::CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

std::optional<Cp15::Callback> Cp15::CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

}  // namespace zb
