#include <cstdint>
#include <variant>

#include "check.h"
#include "zb/cp15.h"

int main() {
    using Dynarmic::A32::CoprocReg;
    std::uint32_t thread_id_ro = 1;
    std::uint32_t thread_id_rw = 2;
    zb::Cp15 cp15(&thread_id_ro, &thread_id_rw);

    const auto frequency = cp15.CompileGetOneWord(false, 0, CoprocReg::C14, CoprocReg::C0, 0);
    CHECK(std::holds_alternative<std::uint32_t*>(frequency));
    CHECK(*std::get<std::uint32_t*>(frequency) == 1000000000u);

    const auto counter = cp15.CompileGetTwoWords(false, 1, CoprocReg::C14);
    CHECK(std::holds_alternative<zb::Cp15::Callback>(counter));
    const auto callback = std::get<zb::Cp15::Callback>(counter);
    const std::uint64_t first = callback.function(nullptr, 0, 0);
    const std::uint64_t second = callback.function(nullptr, 0, 0);
    CHECK(first > 0 && second >= first);

    CHECK(std::holds_alternative<std::monostate>(cp15.CompileGetTwoWords(false, 0, CoprocReg::C14)));
    CHECK(std::holds_alternative<std::monostate>(cp15.CompileGetTwoWords(true, 1, CoprocReg::C14)));
    CHECK(std::holds_alternative<std::monostate>(cp15.CompileGetTwoWords(false, 1, CoprocReg::C13)));
    CHECK(std::holds_alternative<std::monostate>(cp15.CompileGetOneWord(false, 0, CoprocReg::C14, CoprocReg::C0, 1)));
    CHECK(std::get<std::uint32_t*>(cp15.CompileGetOneWord(false, 0, CoprocReg::C13, CoprocReg::C0, 3)) == &thread_id_ro);
}
