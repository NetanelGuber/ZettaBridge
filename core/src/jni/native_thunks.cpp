#include "zb/native_thunks.h"

#include <cstddef>
#include <utility>

extern "C" {
// Both defined in or called from thunks.S.
extern const char zb_native_thunk_base[];
void zb_native_dispatch(std::uint32_t slot, zb::NativeRegs* regs);
}

namespace zb {

namespace {

std::atomic<NativeDispatcher> g_dispatcher{nullptr};

}  // namespace

// thunks.S saves the registers with exactly this layout.
static_assert(offsetof(NativeRegs, x) == 0);
static_assert(offsetof(NativeRegs, d) == 64);
static_assert(offsetof(NativeRegs, stack) == 128);
static_assert(sizeof(NativeRegs) == 144);

void set_native_dispatcher(NativeDispatcher dispatcher) {
    g_dispatcher.store(dispatcher);
}

void* native_thunk_address(std::uint32_t slot) {
    if (slot >= kNativeThunkCount) return nullptr;
    return const_cast<char*>(zb_native_thunk_base) + 8 * static_cast<std::size_t>(slot);
}

NativeSlots::NativeSlots(std::size_t capacity)
    : capacity_(capacity < kNativeThunkCount ? capacity : kNativeThunkCount),
      targets_(new NativeTarget[capacity_]) {}

std::int32_t NativeSlots::allocate(NativeTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t slot = count_.load();
    if (slot >= capacity_) return -1;
    targets_[slot] = std::move(target);
    count_.store(slot + 1);  // published only after the entry is complete
    return static_cast<std::int32_t>(slot);
}

const NativeTarget* NativeSlots::target(std::uint32_t slot) const {
    return slot < count_.load() ? &targets_[slot] : nullptr;
}

}  // namespace zb

void zb_native_dispatch(std::uint32_t slot, zb::NativeRegs* regs) {
    const zb::NativeDispatcher dispatcher = zb::g_dispatcher.load();
    if (dispatcher != nullptr) dispatcher(slot, *regs);
}
