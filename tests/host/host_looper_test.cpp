#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_jvm.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/host_looper.h"
#include "zb/library_runtime.h"
#include "zb/library_protocol.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/proxy_runtime.h"

namespace {

constexpr std::uint32_t kGuestPage = 0x10000;
constexpr std::uint32_t kGuestStack = kGuestPage + 0x800;

std::uint32_t call(zb::HostLooper& looper, zb::GuestThread& thread, std::uint32_t index,
                   std::uint32_t r0 = 0, std::uint32_t r1 = 0, std::uint32_t r2 = 0,
                   std::uint32_t r3 = 0) {
    thread.regs()[0] = r0;
    thread.regs()[1] = r1;
    thread.regs()[2] = r2;
    thread.regs()[3] = r3;
    thread.regs()[13] = kGuestStack;
    CHECK(looper.handle_host_call(index, thread));
    CHECK(thread.regs()[1] == 0);
    return thread.regs()[0];
}

void guest_u32(zb::GuestMemory& memory, std::uint32_t address, std::uint32_t value) {
    std::uint8_t* target = memory.host_ptr(address, sizeof value, zb::kPageWrite);
    CHECK(target != nullptr);
    std::memcpy(target, &value, sizeof value);
}

std::uint32_t guest_u32(zb::GuestMemory& memory, std::uint32_t address) {
    const std::uint8_t* source = memory.host_ptr(address, sizeof(std::uint32_t), zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

std::uint32_t add_fd(zb::HostLooper& looper, zb::GuestThread& thread, zb::GuestMemory& memory,
                     std::uint32_t handle, int fd, int ident, int events,
                     std::uint32_t callback, std::uint32_t data) {
    guest_u32(memory, kGuestStack, callback);
    guest_u32(memory, kGuestStack + 4, data);
    return call(looper, thread, zb::ZB_COMPAT_HC_ALooper_addFd, handle,
                static_cast<std::uint32_t>(fd), static_cast<std::uint32_t>(ident),
                static_cast<std::uint32_t>(events));
}

}  // namespace

void run_unit() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kGuestPage, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(2);
    zb::GuestThread first(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    zb::GuestThread second(runtime.memory(), &monitor, 1, false, zb::kCarrierCodeCacheSize);
    zb::HostLooper looper(runtime);

    CHECK(!looper.handle_host_call(0, first));
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_forThread) == 0);
    const std::uint32_t first_handle =
        call(looper, first, zb::ZB_COMPAT_HC_ALooper_prepare, 1);
    CHECK(first_handle != 0);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_forThread) == first_handle);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_prepare) == first_handle);

    const std::uint32_t second_handle =
        call(looper, second, zb::ZB_COMPAT_HC_ALooper_prepare, 0);
    CHECK(second_handle != 0);
    CHECK(second_handle != first_handle);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_acquire, first_handle) == 0);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_release, first_handle) == 0);

    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 42, 1, 0,
                 0x12345678) == 1);
    // Replacing the same fd must update the identifier and private data.
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 43, 1, 0,
                 0x87654321) == 1);
    const std::uint64_t one = 1;
    CHECK(write(fd, &one, sizeof one) == static_cast<ssize_t>(sizeof one));

    constexpr std::uint32_t kOutFd = kGuestPage + 0x100;
    constexpr std::uint32_t kOutEvents = kGuestPage + 0x104;
    constexpr std::uint32_t kOutData = kGuestPage + 0x108;
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         1000, kOutFd, kOutEvents, kOutData)) == 43);
    CHECK(guest_u32(runtime.memory(), kOutFd) == static_cast<std::uint32_t>(fd));
    CHECK(guest_u32(runtime.memory(), kOutEvents) == 1);
    CHECK(guest_u32(runtime.memory(), kOutData) == 0x87654321);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 1);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 0);

    // removeFd wakes a concurrent poll; consume that wake before checking a true timeout.
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -1);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -3);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_wake, first_handle) == 0);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -1);

    CHECK(add_fd(looper, first, runtime.memory(), 0xdeadbeef, fd, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, -1, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, second, runtime.memory(), second_handle, fd, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, -1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));

    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 44, 1, 0, 0) == 1);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0xdeadbeef, 0, 0)) == -4);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 1);
    CHECK(close(fd) == 0);

    std::puts("host_looper_test PASS");
}

void run_guest(int argc, char** argv) {
    CHECK(argc == 4);
    auto* vm = new zb::mock::MockJvm();
    auto* engine = new zb::GuestJniEngine(*vm);
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.guest_environment = {std::string("LD_LIBRARY_PATH=") + argv[3]};
    options.preload = "libzbjni.so";
    std::string error;
    CHECK(engine->start(options, error));

    zb::LibraryRuntime& runtime = engine->runtime();
    const std::uint32_t library = runtime.load_library(
        std::string(argv[3]) + "/libzblooperprobe.so", ZB_GUEST_RTLD_NOW, error);
    if (library == 0) std::fprintf(stderr, "looper probe load failed: %s\n", error.c_str());
    CHECK(library != 0);
    const std::uint32_t probe = runtime.find_symbol(library, "zb_looper_probe", error);
    CHECK(probe != 0);
    const auto result = runtime.call_on_service(probe, zb::GuestCall{});
    CHECK(result.has_value());
    if (result->r0 != 0) std::fprintf(stderr, "looper probe failed at guest line %u\n", result->r0);
    CHECK(result->r0 == 0);

    std::puts("host_looper_test guest PASS");
    std::fflush(stdout);
    std::_Exit(0);
}

int main(int argc, char** argv) {
    if (argc == 1) {
        run_unit();
        return 0;
    }
    run_guest(argc, argv);
}
