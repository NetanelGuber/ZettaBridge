#pragma once

#include <cstdint>
#include <memory>

namespace zb {

class GuestThread;
class LibraryRuntime;

// Bridges the callback-based Android looper ABI used by arm32 guests. Guest file descriptors
// are process file descriptors, but ALooper objects and callback addresses remain guest values.
class HostLooper {
public:
    explicit HostLooper(LibraryRuntime& runtime);
    ~HostLooper();
    HostLooper(const HostLooper&) = delete;
    HostLooper& operator=(const HostLooper&) = delete;

    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
