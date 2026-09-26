#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_map>

#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"
#include "zb/native_window_backend.h"

namespace zb {

class HostJni;

// ANativeWindow_* host-call dispatcher (Phase 7a Task 5): the eight functions
// core/include/zb/window_hostcalls.h names. Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl, HostAssets and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). ANativeWindow* never crosses into the guest as a host
// pointer: it is a 32-bit handle into the table owned here. An unknown handle returns -1 from
// the query functions and never reaches the backend.
class HostNativeWindow {
public:
    using Allocator = std::function<std::uint32_t(std::uint32_t)>;
    using Deallocator = std::function<void(std::uint32_t)>;
    HostNativeWindow(LibraryRuntime& runtime, NativeWindowBackend& backend, HostJni& host_jni,
                     Allocator allocate = {}, Deallocator deallocate = {})
        : runtime_(runtime), backend_(backend), host_jni_(host_jni),
          allocate_(std::move(allocate)), deallocate_(std::move(deallocate)) {}
    HostNativeWindow(const HostNativeWindow&) = delete;
    HostNativeWindow& operator=(const HostNativeWindow&) = delete;

    // Serves ZB_WINDOW_HC_* indices (160-167); returns false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // The WindowResolver seam HostEgl::eglCreateWindowSurface uses to turn a guest window handle
    // into a host ANativeWindow*; nullptr for an unknown handle.
    const void* value_for(std::uint32_t handle) const;

private:
    const void* require_window(std::uint32_t handle) const;
    void free_guest(std::uint32_t address);
    std::uint32_t allocate_guest(std::uint32_t bytes);
    std::int32_t lock_window(std::uint32_t handle, std::uint32_t out_buffer, std::uint32_t dirty);
    std::int32_t unlock_window(std::uint32_t handle);

    struct LockState {
        std::uint32_t address;
        std::uint64_t bytes;
        void* bits;
        std::thread::id owner;
    };

    LibraryRuntime& runtime_;
    NativeWindowBackend& backend_;
    HostJni& host_jni_;
    Allocator allocate_;
    Deallocator deallocate_;
    GlobalHandles windows_{HandleKind::Global};
    mutable std::mutex surfaces_mutex_;
    std::unordered_map<std::uint32_t, std::uint32_t> references_;
    std::unordered_map<std::uint32_t, LockState> locks_;
};

}  // namespace zb
