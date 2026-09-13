#include "zb/cp15.h"

namespace zb {

using Dynarmic::A32::CoprocReg;

namespace {

bool is_thread_id_reg(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm) {
    return !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0;
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
    if (is_thread_id_reg(two, opc1, CRn, CRm)) {
        if (opc2 == 3) return tpidruro_;
        if (opc2 == 2) return tpidrurw_;
    }
    return std::monostate{};
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool, unsigned, CoprocReg) {
    return std::monostate{};
}

std::optional<Cp15::Callback> Cp15::CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

std::optional<Cp15::Callback> Cp15::CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

}  // namespace zb
