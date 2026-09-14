#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_protocol.h"
#include "zb/native_call.h"
#include "zb/process.h"

namespace zb {

struct LibraryRuntimeOptions {
    std::string zbhost;   // host path of the arm32 zbhost executable
    std::string sysroot;  // host directory with the arm32 system files
    std::uint32_t target_sdk = 0;
    std::vector<std::string> guest_environment;  // guest environment, e.g. LD_LIBRARY_PATH=...
    std::string preload;            // guest path dlopen'ed RTLD_GLOBAL before READY; empty: none
    std::chrono::milliseconds ready_timeout{10000};
};

// A guest process in library mode: zbhost runs on a service guest thread that owns the boot
// JIT and serves requests from inside its READY host call.
//
// The runtime is process-lifetime. Guest threads cannot be torn down, so after a successful
// start() the destructor logs and aborts; tests end with std::_Exit.
class LibraryRuntime {
public:
    LibraryRuntime();
    ~LibraryRuntime();
    LibraryRuntime(const LibraryRuntime&) = delete;
    LibraryRuntime& operator=(const LibraryRuntime&) = delete;

    // Receives every host call outside the runtime range 0xFE00-0xFEFF. Call before start().
    void set_host_call_handler(Process::HostCallHandler handler);
    bool start(const LibraryRuntimeOptions& options, std::string& error);

    // Guest dlopen/dlsym/dlerror on the service thread. Flags are ZB_GUEST_RTLD_* values.
    // Return 0 and set error (guest dlerror text) on failure.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::optional<GuestResult> call_on_service(std::uint32_t function, const GuestCall& args);
    // Nested call on the guest thread the calling host thread already runs (for host-call
    // handlers). nullopt if the calling host thread runs no guest code.
    std::optional<GuestResult> call_on_current(std::uint32_t function, const GuestCall& args);

    GuestMemory& memory();
    // Valid after a successful start().
    const zb_service_api& service_api() const;
    // Real guest pthreads (service, carriers, guest-created threads); borrowers are not counted.
    std::size_t guest_thread_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
