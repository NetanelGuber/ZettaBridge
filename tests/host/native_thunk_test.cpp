// Calls precompiled thunks like JNI functions and checks what the dispatcher receives: the slot
// number, x0-x7, d0-d7, host stack arguments, and the result coming back through x0 / d0.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "zb/native_thunks.h"

namespace {

std::uint32_t g_slot = 0xFFFFFFFF;
zb::NativeRegs g_seen{};

void record(std::uint32_t slot, zb::NativeRegs& regs) {
    g_slot = slot;
    g_seen = regs;
    regs.x[0] = 0x1122334455667788ull;
    const double result = 2.5;
    std::memcpy(&regs.d[0], &result, 8);
}

}  // namespace

int main() {
    zb::set_native_dispatcher(record);

    // Ten integer-class arguments: x0-x7 take env, class and six ints; 8 and 9 go on the stack.
    using Wide = std::int64_t (*)(void*, void*, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                  std::int64_t, std::int64_t, std::int64_t, std::int64_t, float, double);
    const auto wide = reinterpret_cast<Wide>(zb::native_thunk_address(5));
    const std::int64_t result = wide(reinterpret_cast<void*>(0xE0), reinterpret_cast<void*>(0xC1), 2, 3, 4, 5,
                                     6, 7, 8, 9, 1.5f, 3.25);
    CHECK(g_slot == 5);
    CHECK(result == 0x1122334455667788ll);
    CHECK(g_seen.x[0] == 0xE0 && g_seen.x[1] == 0xC1 && g_seen.x[2] == 2 && g_seen.x[7] == 7);
    CHECK(g_seen.stack[0] == 8 && g_seen.stack[1] == 9);
    float f;
    std::memcpy(&f, &g_seen.d[0], 4);
    CHECK(f == 1.5f);
    double d;
    std::memcpy(&d, &g_seen.d[1], 8);
    CHECK(d == 3.25);

    // The last thunk, returning a double through d0.
    using Fp = double (*)(void*, void*);
    const auto last = reinterpret_cast<Fp>(zb::native_thunk_address(zb::kNativeThunkCount - 1));
    CHECK(last(nullptr, nullptr) == 2.5);
    CHECK(g_slot == zb::kNativeThunkCount - 1);
    CHECK(zb::native_thunk_address(zb::kNativeThunkCount) == nullptr);

    zb::NativeSlots slots(2);
    CHECK(slots.allocate({0x10001, "VI", true}) == 0);
    CHECK(slots.allocate({0x20000, "IIFFIFF", false}) == 1);
    CHECK(slots.allocate({0x30000, "V", true}) == -1);  // exhausted
    CHECK(slots.target(1) != nullptr && slots.target(1)->shorty == "IIFFIFF" && !slots.target(1)->is_static);
    CHECK(slots.target(2) == nullptr);

    std::printf("native_thunk_test ok\n");
    return 0;
}
