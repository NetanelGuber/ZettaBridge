#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "zb/bitmap_backend.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;

// AndroidBitmap_* bridge. Pixel pointers are guest allocations copied back on unlock; host
// jobjects are retained while locked and matched by JNI object identity, not pointer value.
class HostBitmap {
public:
    using Context = std::function<std::pair<JniBackend::Env, JniBackend::Ref>(std::uint32_t)>;
    using Allocator = std::function<std::uint32_t(std::uint32_t)>;
    using Deallocator = std::function<void(std::uint32_t)>;
    HostBitmap(LibraryRuntime& runtime, HostJni& jni, BitmapBackend& backend,
               Context context = {}, Allocator allocate = {}, Deallocator deallocate = {})
        : runtime_(runtime), jni_(jni), backend_(backend), context_(std::move(context)),
          allocate_(std::move(allocate)), deallocate_(std::move(deallocate)) {}
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    struct Lock {
        JniBackend::Ref bitmap;
        std::uint32_t address;
        std::uint64_t bytes;
        void* pixels;
        std::thread::id owner;
    };
    void free_guest(std::uint32_t address);
    std::uint32_t allocate_guest(std::uint32_t bytes);

    LibraryRuntime& runtime_;
    HostJni& jni_;
    BitmapBackend& backend_;
    Context context_;
    Allocator allocate_;
    Deallocator deallocate_;
    std::recursive_mutex mutex_;
    std::vector<Lock> locks_;
};

}  // namespace zb
