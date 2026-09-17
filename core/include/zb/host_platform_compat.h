#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "zb/guest_thread.h"

namespace zb {

// Safe loader-compatibility fallbacks for platform entry points whose full pointer bridges are
// deferred. Every handled call is reported as unimplemented; pointer-returning or output-writing
// APIs return an explicit failure instead of letting the generic r0=0 path claim success.
class HostPlatformCompat {
public:
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    std::uint32_t looper_for_thread(GuestThread& thread, bool prepare);
    void acquire_looper(std::uint32_t handle);
    void release_looper(std::uint32_t handle);

    std::mutex looper_mutex_;
    std::unordered_map<const GuestThread*, std::uint32_t> thread_loopers_;
    std::unordered_map<std::uint32_t, std::uint32_t> looper_refs_;
    std::uint32_t next_looper_ = 0x7a000000u;
};

}  // namespace zb
