#pragma once

#include <cstdint>
#include <mutex>

#include "zb/configuration_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostAssets;

class HostConfiguration {
public:
    HostConfiguration(LibraryRuntime& runtime, ConfigurationBackend& backend, HostAssets* assets)
        : runtime_(runtime), backend_(backend), assets_(assets) {}
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    LibraryRuntime& runtime_;
    ConfigurationBackend& backend_;
    HostAssets* assets_;
    GlobalHandles configs_{HandleKind::Global};
    std::mutex mutex_;
};

}  // namespace zb
