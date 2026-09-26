#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_jvm.h"
#include "zb/guest_memory.h"
#include "zb/host_bitmap.h"
#include "zb/host_jni.h"
#include "zb/platform_compat_hostcalls.h"

namespace {
class MockBitmap final : public zb::BitmapBackend {
public:
    int get_info(zb::JniBackend::Env, zb::JniBackend::Ref, zb::BitmapInfo& info) override {
        info = {2, 2, 8, 1, 0};
        return 0;
    }
    int lock_pixels(zb::JniBackend::Env, zb::JniBackend::Ref, void*& address) override {
        CHECK(!locked);
        locked = true;
        address = pixels.data();
        return 0;
    }
    int unlock_pixels(zb::JniBackend::Env, zb::JniBackend::Ref) override {
        CHECK(locked);
        locked = false;
        return 0;
    }
    zb::JniBackend::Ref retain(zb::JniBackend::Env, zb::JniBackend::Ref bitmap) override {
        ++retained;
        return bitmap;
    }
    void release(zb::JniBackend::Env, zb::JniBackend::Ref) override { --retained; }
    bool same_object(zb::JniBackend::Env, zb::JniBackend::Ref a, zb::JniBackend::Ref b) override {
        return a == b;
    }
    bool locked = false;
    int retained = 0;
    std::array<std::uint8_t, 16> pixels{};
};

std::int32_t call(zb::HostBitmap& bridge, zb::GuestThread& thread, std::uint32_t index,
                  std::uint32_t object, std::uint32_t output = 0) {
    thread.regs()[0] = 0;
    thread.regs()[1] = object;
    thread.regs()[2] = output;
    CHECK(bridge.handle_host_call(index, thread));
    CHECK(thread.regs()[1] == 0);
    return static_cast<std::int32_t>(thread.regs()[0]);
}
}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(0x10000, 0x2000, PROT_READ | PROT_WRITE));
    zb::mock::MockJvm vm;
    auto* jni = new zb::HostJni(runtime, vm);
    MockBitmap backend;
    std::uint32_t next = 0x11000;
    unsigned frees = 0;
    zb::HostBitmap bridge(runtime, *jni, backend,
        [](std::uint32_t object) { return std::pair<zb::JniBackend::Env, zb::JniBackend::Ref>{1, object == 2 ? 1 : object}; },
        [&](std::uint32_t bytes) { const auto result = next; next += bytes; return result; },
        [&](std::uint32_t) { ++frees; });
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    CHECK(!bridge.handle_host_call(0, thread));
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_getInfo, 1, 0xFFFF0000) == -1);
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_getInfo, 1, 0x10000) == 0);
    zb::BitmapInfo info;
    std::memcpy(&info, runtime.memory().host_ptr(0x10000, sizeof info, zb::kPageRead), sizeof info);
    CHECK(info.width == 2 && info.height == 2 && info.stride == 8 && info.format == 1);
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_lockPixels, 1, 0x10020) == 0);
    CHECK(backend.locked && backend.retained == 1);
    std::uint32_t address = 0;
    std::memcpy(&address, runtime.memory().host_ptr(0x10020, 4, zb::kPageRead), 4);
    CHECK(address == 0x11000);
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_lockPixels, 2, 0x10020) == -1);
    auto* guest = runtime.memory().host_ptr(address, 16, zb::kPageWrite);
    guest[0] = 77;
    std::int32_t wrong_thread = 0;
    std::thread other([&] {
        Dynarmic::ExclusiveMonitor other_monitor(1);
        zb::GuestThread other_guest(runtime.memory(), &other_monitor, 0, false, zb::kCarrierCodeCacheSize);
        wrong_thread = call(bridge, other_guest, zb::ZB_COMPAT_HC_AndroidBitmap_unlockPixels, 2);
    });
    other.join();
    CHECK(wrong_thread == -1 && backend.locked);
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_unlockPixels, 2) == 0);
    CHECK(!backend.locked && backend.retained == 0 && backend.pixels[0] == 77 && frees == 1);
    CHECK(call(bridge, thread, zb::ZB_COMPAT_HC_AndroidBitmap_unlockPixels, 1) == -1);
    std::puts("host_bitmap_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
