#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_jvm.h"
#include "mock_looper.h"
#include "zb/guest_memory.h"
#include "zb/host_input.h"
#include "zb/host_jni.h"
#include "zb/host_looper.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/runtime_report.h"

namespace {
struct Call { std::uint32_t index; const char* library; const char* name; };
constexpr Call kCalls[] = {
#include "../../core/src/gen/hostcalls.inc"
};
std::uint32_t index_of(std::string_view name) {
    for (const auto& call : kCalls) if (name == call.name) return call.index;
    std::abort();
}

class MockInput final : public zb::InputBackend {
public:
    MockInput() { fd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK); CHECK(fd >= 0); }
    ~MockInput() override { close(fd); }
    bool from_java_available() override { return available; }
    std::uint64_t from_java(zb::JniBackend::Env env, zb::JniBackend::Ref object) override {
        CHECK(env == 1 && object == 7);
        return 0x100000007ull;
    }
    int readiness_fd(std::uint64_t queue) override { CHECK(queue == 0x100000007ull); return fd; }
    int has_events(std::uint64_t) override { return pending ? 1 : 0; }
    int get_event(std::uint64_t, std::uint64_t& event) override {
        if (!pending) return -EAGAIN;
        pending = false;
        event = event_native;
        std::uint64_t value;
        CHECK(read(fd, &value, 8) == 8);
        return 0;
    }
    int pre_dispatch(std::uint64_t, std::uint64_t) override { return predispatch; }
    void finish_event(std::uint64_t, std::uint64_t event, int handled) override {
        CHECK(event == event_native && handled == 1);
        ++finished;
    }
    bool query(const char* name, std::uint64_t event, std::uint32_t a,
               std::uint32_t b, std::uint32_t, char, std::uint64_t& bits) override {
        CHECK(event == event_native);
        const std::string_view n(name);
        if (n == "AInputEvent_getType") bits = event_type;
        else if (n == "AKeyEvent_getKeyCode" && event_type == 1) bits = 42;
        else if (n == "AKeyEvent_getEventTime" && event_type == 1) bits = 0x123456789ull;
        else if (n == "AMotionEvent_getPointerCount" && event_type == 2) bits = 2;
        else if (n == "AMotionEvent_getHistorySize" && event_type == 2) bits = 1;
        else if (n == "AMotionEvent_getX" && event_type == 2) {
            CHECK(a == 1);
            bits = std::bit_cast<std::uint32_t>(12.5f);
        } else if (n == "AMotionEvent_getHistoricalX" && event_type == 2) {
            CHECK(a == 1 && b == 0);
            bits = std::bit_cast<std::uint32_t>(7.5f);
        }
        else if (n == "AMotionEvent_getEventTime" && event_type == 2) bits = 0x23456789aull;
        else return false;
        return true;
    }
    int fd;
    bool pending = true;
    int predispatch = 0;
    int finished = 0;
    bool available = true;
    int event_type = 1;
    std::uint64_t event_native = 0x200000001ull;
};

std::int32_t call(zb::HostInput& input, zb::GuestThread& thread, std::uint32_t index,
                  std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0,
                  std::uint32_t d = 0) {
    auto& r = thread.regs();
    r[0] = a; r[1] = b; r[2] = c; r[3] = d; r[13] = 0x10800;
    CHECK(input.handle_host_call(index, thread));
    return static_cast<std::int32_t>(r[0]);
}
}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    zb::mock::MockJvm vm;
    auto* jni = new zb::HostJni(runtime, vm);
    zb::HostLooper looper(runtime);
    MockInput backend;
    zb::HostInput input(runtime, *jni, looper, backend,
        [](std::uint32_t object) { return std::pair<zb::JniBackend::Env, zb::JniBackend::Ref>{1, object}; });
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread guest(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    CHECK(!input.handle_host_call(0, guest));
    CHECK(call(input, guest, 400, 0xdeadbeef) == -EINVAL);
    CHECK(call(input, guest, 398, 0, 7) > 0);
    const auto queue = guest.regs()[0];
    CHECK(queue != 0x100000007ull);
    CHECK(call(input, guest, 398, 0, 7) == static_cast<int>(queue));
    CHECK(call(input, guest, 400, queue) == 1);
    CHECK(call(input, guest, 399, queue, 0xffff0000) == -EFAULT);
    const auto loop = [&] {
        guest.regs()[0] = 1;
        CHECK(looper.handle_host_call(zb::ZB_COMPAT_HC_ALooper_prepare, guest));
        return guest.regs()[0];
    }();
    std::uint32_t data = 0x55;
    std::memcpy(runtime.memory().host_ptr(0x10800, 4, zb::kPageWrite), &data, 4);
    CHECK(call(input, guest, 395, queue, loop, 17, 0) == 0);
    guest.regs()[0] = 0; guest.regs()[1] = 0; guest.regs()[2] = 0; guest.regs()[3] = 0;
    CHECK(looper.handle_host_call(zb::ZB_COMPAT_HC_ALooper_pollOnce, guest));
    CHECK(guest.regs()[0] == 17);
    CHECK(call(input, guest, 399, queue, 0x10000) == 0);
    std::uint32_t event;
    std::memcpy(&event, runtime.memory().host_ptr(0x10000, 4, zb::kPageRead), 4);
    CHECK(event != 0 && event != 0x200000001ull);
    CHECK(call(input, guest, index_of("AInputEvent_getType"), event) == 1);
    CHECK(call(input, guest, index_of("AKeyEvent_getKeyCode"), event) == 42);
    CHECK(call(input, guest, index_of("AKeyEvent_getEventTime"), event) == 0x23456789);
    CHECK(guest.regs()[1] == 1);
    CHECK(call(input, guest, index_of("AMotionEvent_getAction"), event) == -EINVAL);
    std::int32_t wrong_thread = 0;
    std::thread other([&] {
        Dynarmic::ExclusiveMonitor second_monitor(1);
        zb::GuestThread second(runtime.memory(), &second_monitor, 0, false, zb::kCarrierCodeCacheSize);
        wrong_thread = call(input, second, index_of("AKeyEvent_getKeyCode"), event);
    });
    other.join();
    CHECK(wrong_thread == -EINVAL);
    CHECK(call(input, guest, 397, queue, event, 1) == 0);
    CHECK(backend.finished == 1);
    CHECK(call(input, guest, index_of("AKeyEvent_getKeyCode"), event) == -EINVAL);
    CHECK(call(input, guest, 399, queue, 0x10000) == -EAGAIN);
    CHECK(call(input, guest, 396, queue) == 0);

    backend.event_type = 2;
    backend.event_native = 0x200000002ull;
    backend.pending = true;
    const std::uint64_t motion_signal = 1;
    CHECK(write(backend.fd, &motion_signal, 8) == 8);
    CHECK(call(input, guest, 399, queue, 0x10000) == 0);
    std::memcpy(&event, runtime.memory().host_ptr(0x10000, 4, zb::kPageRead), 4);
    CHECK(call(input, guest, index_of("AInputEvent_getType"), event) == 2);
    CHECK(call(input, guest, index_of("AMotionEvent_getPointerCount"), event) == 2);
    CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(
        call(input, guest, index_of("AMotionEvent_getX"), event, 1))) == 12.5f);
    CHECK(std::isnan(std::bit_cast<float>(static_cast<std::uint32_t>(
        call(input, guest, index_of("AMotionEvent_getX"), event, 2)))));
    CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(
        call(input, guest, index_of("AMotionEvent_getHistoricalX"), event, 1, 0))) == 7.5f);
    CHECK(std::isnan(std::bit_cast<float>(static_cast<std::uint32_t>(
        call(input, guest, index_of("AMotionEvent_getHistoricalX"), event, 1, 1)))));
    CHECK(call(input, guest, index_of("AMotionEvent_getEventTime"), event) == 0x3456789a);
    CHECK(guest.regs()[1] == 2);
    CHECK(call(input, guest, index_of("AKeyEvent_getKeyCode"), event) == -EINVAL);
    CHECK(call(input, guest, 397, queue, event, 1) == 0);
    CHECK(backend.finished == 2);
    backend.event_type = 1;
    backend.event_native = 0x200000001ull;

    // A Java borrower has a real looper. Its callback re-enters the guest and a zero return
    // unregisters this queue's notification fd, exactly as for an ordinary addFd callback.
    MockAndroidLooper real;
    int callbacks = 0;
    zb::HostLooper attached_looper(runtime, &real,
        [&](std::uint32_t function, const zb::GuestCall& args) -> std::optional<zb::GuestResult> {
            CHECK(function == 0x2000 && args.regs[0] == static_cast<std::uint32_t>(backend.fd));
            CHECK(args.regs[1] == 1 && args.regs[2] == 0xaa);
            ++callbacks;
            zb::GuestResult result;
            result.r0 = 0;
            return result;
        }, [](const zb::GuestThread&) { return true; });
    zb::HostInput attached_input(runtime, *jni, attached_looper, backend,
        [](std::uint32_t object) { return std::pair<zb::JniBackend::Env, zb::JniBackend::Ref>{1, object}; });
    guest.regs()[0] = 0;
    CHECK(attached_looper.handle_host_call(zb::ZB_COMPAT_HC_ALooper_prepare, guest));
    const auto attached_handle = guest.regs()[0];
    CHECK(attached_handle != 0 && real.current() != 0);
    const auto attached_queue = static_cast<std::uint32_t>(call(attached_input, guest, 398, 0, 7));
    CHECK(call(attached_input, guest, 395, attached_queue, 0xdeadbeef, -1, 0x2000) == -EINVAL);
    data = 0xaa;
    std::memcpy(runtime.memory().host_ptr(0x10800, 4, zb::kPageWrite), &data, 4);
    CHECK(call(attached_input, guest, 395, attached_queue, attached_handle, -1, 0x2000) == 0);
    CHECK(real.registered(real.current(), backend.fd));
    CHECK(real.deliver(real.current(), backend.fd, 1) == 0);
    CHECK(callbacks == 1 && !real.registered(real.current(), backend.fd));
    CHECK(call(attached_input, guest, 396, attached_queue) == 0);
    backend.pending = true;
    backend.predispatch = 1;
    const std::uint64_t one = 1;
    CHECK(write(backend.fd, &one, 8) == 8);
    CHECK(call(attached_input, guest, 399, attached_queue, 0x10000) == 0);
    std::memcpy(&event, runtime.memory().host_ptr(0x10000, 4, zb::kPageRead), 4);
    CHECK(call(attached_input, guest, 401, attached_queue, event) == 1);
    CHECK(call(attached_input, guest, index_of("AKeyEvent_getKeyCode"), event) == -EINVAL);
    zb::runtime_report().clear();
    CHECK(call(input, guest, index_of("AInputEvent_toJava"), 0, event) == 0);
    backend.available = false;
    CHECK(call(input, guest, 398, 0, 7) == 0);
    CHECK(zb::runtime_report().unimplemented_host_calls() == 2);
    CHECK(zb::runtime_report().text().find("AInputQueue_fromJava") != std::string::npos);
    std::puts("host_input_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
