#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <unordered_map>

#include "zb/asset_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;

// AAsset* host-call dispatcher. Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). AAssetManager* and AAsset* never cross into the guest as host
// pointers: they are 32-bit handles into handle tables owned here.
class HostAssets {
public:
    using Allocator = std::function<std::uint32_t(std::uint32_t)>;
    using Deallocator = std::function<void(std::uint32_t)>;
    HostAssets(LibraryRuntime& runtime, AssetBackend& backend, HostJni& host_jni,
               Allocator allocate = {}, Deallocator deallocate = {})
        : runtime_(runtime), backend_(backend), host_jni_(host_jni),
          allocate_(std::move(allocate)), deallocate_(std::move(deallocate)) {}
    HostAssets(const HostAssets&) = delete;
    HostAssets& operator=(const HostAssets&) = delete;

    // Serves the complete generated 142-159 asset range; false outside it.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    // Borrowed opaque host value for AConfiguration_fromAssetManager; never guest-visible.
    std::uint64_t manager_value(std::uint32_t handle) const { return require_manager(handle); }

private:
    std::uint64_t require_manager(std::uint32_t handle) const;
    std::uint64_t require_asset(std::uint32_t handle) const;
    void free_guest(std::uint32_t address);
    std::uint32_t allocate_guest(std::uint32_t size);
    std::uint32_t copy_to_guest(const void* source, std::uint64_t size);

    LibraryRuntime& runtime_;
    AssetBackend& backend_;
    HostJni& host_jni_;
    // Serializes asset operations with close, including guest buffer lifetime. Recursive only
    // because a guest allocator invoked by getBuffer may itself issue a nested asset call.
    std::recursive_mutex mutex_;
    Allocator allocate_;
    Deallocator deallocate_;
    // AAsset_getBuffer copies, since the NDK buffer lives outside the guest address space. An
    // asset is read-only, so one copy per asset is enough and never needs writing back.
    std::uint32_t asset_buffer(std::uint64_t asset);

    GlobalHandles managers_{HandleKind::Global};
    GlobalHandles assets_{HandleKind::Global};
    GlobalHandles directories_{HandleKind::Global};
    bool logged_overflow_ = false;
    static constexpr std::uint64_t kMaxBufferedBytes = 64ull << 20;
    std::unordered_map<std::uint64_t, std::uint32_t> asset_buffers_;
    std::unordered_map<std::uint64_t, std::uint32_t> directory_names_;
    std::uint64_t buffered_bytes_ = 0;
};

}  // namespace zb
