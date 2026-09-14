// Library-mode runtime on the service thread: guest RTLD flags, dlopen/dlsym/dlerror, calls of
// every return type, host-call chaining, misuse guards, preload failure, and signal delivery
// to the parked service thread.
// Usage: library_runtime_test <sysroot> <zbhost> <libzbcallprobe.so>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace {

// The wrapper executes in a signal handler: only always-lock-free atomics are allowed.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> hold_signal_reader{false};
std::atomic<bool> signal_reader_entered{false};
std::atomic<bool> release_signal_reader{false};
// Acquire-order stress: a forwarding reader must never use a target whose retirement completed.
std::atomic<bool> stress_active{false};
std::atomic<bool> stress_target_retired{true};
std::atomic<bool> stress_violation{false};
std::atomic<std::uint64_t> stress_posts{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

}  // namespace

extern "C" void real_post_signal(zb::GuestThread*, const zb::g::siginfo32&)
    asm("__real__ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E");
extern "C" void wrapped_post_signal(zb::GuestThread*, const zb::g::siginfo32&)
    asm("__wrap__ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E");

extern "C" void wrapped_post_signal(zb::GuestThread* thread, const zb::g::siginfo32& info) {
    if (hold_signal_reader.load() && info.si_signo == SIGALRM) {
        signal_reader_entered.store(true);
        while (!release_signal_reader.load()) {
        }
    }
    if (stress_active.load() && info.si_signo == SIGUSR1) {
        if (stress_target_retired.load()) stress_violation.store(true);
        stress_posts.fetch_add(1);
    }
    real_post_signal(thread, info);
}

namespace {

using namespace std::chrono_literals;

void check_signal_target_retirement() {
    zb::GuestMemory memory;
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread target(memory, &monitor, 0);
    zb::Process::install_host_signal_forwarding();
    zb::Process::set_process_signal_target(&target);
    hold_signal_reader.store(true);
    std::thread sender([] { CHECK(raise(SIGALRM) == 0); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!signal_reader_entered.load()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }

    // The handler has acquired target but has not used it yet. Retirement must wait.
    std::promise<void> started;
    auto retiring = std::async(std::launch::async, [&] {
        started.set_value();
        zb::Process::clear_process_signal_target(&target);
    });
    started.get_future().wait();
    CHECK(retiring.wait_for(100ms) == std::future_status::timeout);
    release_signal_reader.store(true);
    sender.join();
    CHECK(retiring.wait_for(2s) == std::future_status::ready);
    retiring.get();
    hold_signal_reader.store(false);
    zb::g::siginfo32 info;
    CHECK(target.take_signal(0, info) && info.si_signo == SIGALRM);
    // After quiescence, new handlers cannot acquire the retired target.
    CHECK(raise(SIGALRM) == 0);
    CHECK(!target.take_signal(0, info));
}

// The held reader above is already counted when it blocks inside post_signal, so it cannot tell
// whether a handler registers before or after loading the target. Here readers on several threads
// race a publish/retire loop. stress_target_retired is cleared before each publication and set only
// after clear_process_signal_target() returns, so with correct ordering no post_signal can observe
// it set. A handler that loads the target before registering can run its post_signal after the
// retirement already finished; the loop stops at the first such violation.
void check_signal_target_acquire_order() {
    zb::GuestMemory memory;
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread target(memory, &monitor, 0);
    zb::Process::install_host_signal_forwarding();
    stress_violation.store(false);
    stress_posts.store(0);
    stress_target_retired.store(true);
    stress_active.store(true);
    const unsigned cpus = std::thread::hardware_concurrency();
    const unsigned readers = cpus > 2 ? (cpus - 1 < 4 ? cpus - 1 : 4) : 2;
    std::vector<std::thread> senders;
    std::atomic<bool> stop{false};
    for (unsigned i = 0; i < readers; ++i) {
        senders.emplace_back([&] {
            while (!stop.load()) CHECK(raise(SIGUSR1) == 0);
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!stress_violation.load() && std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) {
            stress_target_retired.store(false);
            zb::Process::set_process_signal_target(&target);
            zb::Process::clear_process_signal_target(&target);
            stress_target_retired.store(true);
        }
    }
    stop.store(true);
    for (auto& sender : senders) sender.join();
    stress_active.store(false);
    std::fprintf(stderr, "signal target stress: %u readers, %llu posts\n", readers,
                 static_cast<unsigned long long>(stress_posts.load()));
    CHECK(!stress_violation.load());
    CHECK(stress_posts.load() > 0);
}

std::uint32_t fbits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t dbits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint32_t read32(zb::LibraryRuntime& runtime, std::uint32_t address) {
    const std::uint8_t* source = runtime.memory().host_ptr(address, 4, zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

// Polls a guest word without running guest code on this host thread.
bool wait_nonzero(zb::LibraryRuntime& runtime, std::uint32_t address, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (read32(runtime, address) == 0) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

zb::LibraryRuntimeOptions runtime_options(char** argv) {
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.guest_environment = {"LD_LIBRARY_PATH=" + std::filesystem::path(argv[3]).parent_path().string()};
    return options;
}

// start() reports a failed preload; runs in a child because a runtime is process-lifetime.
void check_preload_failure(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        {
            zb::LibraryRuntime runtime;
            zb::LibraryRuntimeOptions options = runtime_options(argv);
            options.preload = "libzb-does-not-exist.so";
            std::string error;
            if (runtime.start(options, error)) std::_Exit(10);
            std::fprintf(stderr, "expected start error: %s\n", error.c_str());
            CHECK(error.find("status 4") != std::string::npos);
        }
        // Ordinary failed-start destruction must retire the process signal target.
        CHECK(raise(SIGALRM) == 0);
        std::_Exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--signal-target-retirement") {
        check_signal_target_retirement();
        check_signal_target_acquire_order();
        return 0;
    }
    CHECK(argc == 4);
    const std::string probe = argv[3];
    const std::string libdir = std::filesystem::path(probe).parent_path();
    check_preload_failure(argv);
    check_signal_target_retirement();
    check_signal_target_acquire_order();

    CHECK(ZB_GUEST_RTLD_NOW == 0u && ZB_GUEST_RTLD_LAZY == 1u && ZB_GUEST_RTLD_GLOBAL == 2u);
    CHECK(ZB_GUEST_RTLD_NOLOAD == 4u && ZB_GUEST_RTLD_NODELETE == 0x1000u && ZB_GUEST_RTLD_DEFAULT == 0xFFFFFFFFu);

    zb::LibraryRuntime runtime;
    std::uint32_t chained_function = 0;
    int chained_calls = 0;
    runtime.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFD00) return false;
        ++chained_calls;
        std::string misuse;
        CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW, misuse) == 0);
        CHECK(misuse.find("service thread") != std::string::npos);
        const auto nested = runtime.call_on_current(chained_function, zb::GuestCall{});
        CHECK(nested && nested->r0 == 42);
        thread.regs()[0] += nested->r0;
        return true;
    });

    std::string error;
    CHECK(runtime.start(runtime_options(argv), error));
    CHECK(runtime.guest_thread_count() == 1);
    CHECK(runtime.service_api().version == ZB_SERVICE_PROTOCOL_VERSION);
    CHECK(!runtime.call_on_current(runtime.service_api().malloc_fn, zb::GuestCall{}));

    CHECK(runtime.load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == 0);
    error.clear();
    const std::uint32_t library = runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library);
    CHECK(runtime.find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    error.clear();
    CHECK(runtime.find_symbol(ZB_GUEST_RTLD_DEFAULT, "zb_return_i", error) == symbol("zb_return_i"));

    // Every JNI return type, converted with store_native_result.
    struct ReturnCase {
        const char* name;
        char type;
        std::uint64_t x0;
        std::uint64_t d0;
    };
    const ReturnCase returns[] = {
        {"zb_return_z", 'Z', 1, 0},
        {"zb_return_b", 'B', static_cast<std::uint64_t>(-2), 0},
        {"zb_return_c", 'C', 0x1234, 0},
        {"zb_return_s", 'S', static_cast<std::uint64_t>(-3), 0},
        {"zb_return_i", 'I', 42, 0},
        {"zb_return_j", 'J', 0x1122334455667788ull, 0},
        {"zb_return_f", 'F', 0, fbits(3.5f)},
        {"zb_return_d", 'D', 0, dbits(-1.25)},
        {"zb_return_l", 'L', 0x7000012345ull, 0},
    };
    CHECK(runtime.call_on_service(symbol("zb_return_v"), zb::GuestCall{}));
    for (const ReturnCase& expected : returns) {
        const auto result = runtime.call_on_service(symbol(expected.name), zb::GuestCall{});
        CHECK(result);
        zb::NativeRegs regs{};
        zb::store_native_result(expected.type, result->r0, result->r1, regs,
                                [](std::uint32_t handle) { return 0x7000000000ull | handle; });
        if (expected.type == 'F' || expected.type == 'D') {
            CHECK(regs.d[0] == expected.d0);
        } else {
            CHECK(regs.x[0] == expected.x0);
        }
    }

    // softfp argument layout of a mixed JNI signature.
    std::uint64_t host_stack[1] = {0x99};
    zb::NativeRegs host{};
    host.x[1] = 0x77;
    host.x[2] = 1;
    host.x[3] = static_cast<std::uint64_t>(-2);
    host.x[4] = 0x1234;
    host.x[5] = static_cast<std::uint64_t>(-3);
    host.x[6] = 4;
    host.x[7] = 0x1122334455667788ull;
    host.d[0] = fbits(1.5f);
    host.d[1] = dbits(-2.25);
    host.stack = host_stack;
    const auto to_handle = [](std::uint64_t reference) {
        return reference == 0 ? 0u : static_cast<std::uint32_t>(reference + 0x1000);
    };
    const zb::GuestCall mixed = zb::marshal_native_args("IZBCSIJFDL", host, 0xE000, to_handle);
    const auto mixed_result = runtime.call_on_service(symbol("zb_probe_mix"), mixed);
    CHECK(mixed_result && mixed_result->r0 == 42);

    // Host calls outside the runtime range reach the chained handler, which may nest a call.
    chained_function = symbol("zb_return_i");
    zb::GuestCall value;
    value.regs = {100, 0, 0, 0};
    const auto chained = runtime.call_on_service(symbol("zb_probe_host_call"), value);
    CHECK(chained && chained->r0 == 142 && chained_calls == 1);

    // A process-directed SIGALRM reaches the service thread while it is parked in READY.
    const std::uint32_t alarm_count = symbol("zb_alarm_count");
    const std::uint32_t alarm_tid = symbol("zb_alarm_tid");
    const auto service_tid = runtime.call_on_service(symbol("zb_probe_tid"), zb::GuestCall{});
    CHECK(service_tid && service_tid->r0 != 0);
    zb::GuestCall alarm;
    alarm.regs = {20000, 0, 0, 0};
    const auto armed = runtime.call_on_service(symbol("zb_probe_arm_alarm"), alarm);
    CHECK(armed && armed->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);
    const auto after_alarm = runtime.call_on_service(symbol("zb_return_i"), zb::GuestCall{});
    CHECK(after_alarm && after_alarm->r0 == 42);

    CHECK(runtime.guest_thread_count() == 1);
    std::puts("library_runtime_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
