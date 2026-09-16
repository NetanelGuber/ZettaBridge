#pragma once

#include <cstdint>

#include "zb/asset_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;

// AAsset* host-call dispatcher (Phase 5 Task 7): the six functions liblime.so imports
// (core/include/zb/asset_hostcalls.h). Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). AAssetManager* and AAsset* never cross into the guest as host
// pointers: they are 32-bit handles into handle tables owned here.
class HostAssets {
public:
    HostAssets(LibraryRuntime& runtime, AssetBackend& backend, HostJni& host_jni)
        : runtime_(runtime), backend_(backend), host_jni_(host_jni) {}
    HostAssets(const HostAssets&) = delete;
    HostAssets& operator=(const HostAssets&) = delete;

    // Serves ZB_ASSET_HC_* indices; returns false for any other index (including the other,
    // unimplemented AAsset* stubs in the 142-160 range).
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    std::uint64_t require_manager(std::uint32_t handle) const;
    std::uint64_t require_asset(std::uint32_t handle) const;

    LibraryRuntime& runtime_;
    AssetBackend& backend_;
    HostJni& host_jni_;
    GlobalHandles managers_{HandleKind::Global};
    GlobalHandles assets_{HandleKind::Global};
    bool logged_overflow_ = false;
};

}  // namespace zb
