#include "zb/host_native_window.h"

#include <cstdint>
#include <cerrno>
#include <cstring>
#include <limits>

#include "zb/guest_memory.h"
#include "zb/library_runtime.h"
#include "zb/log.h"
#include "zb/platform_compat_hostcalls.h"

#include "zb/host_jni.h"
#include "zb/window_hostcalls.h"

namespace zb {

namespace {

inline void* as_pointer(std::uint64_t value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

inline std::uint64_t from_pointer(const void* value) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

}  // namespace

const void* HostNativeWindow::value_for(std::uint32_t handle) const {
    const std::optional<std::uint64_t> value = windows_.get(handle);
    if (!value || *value == 0) return nullptr;
    return as_pointer(*value);
}

const void* HostNativeWindow::require_window(std::uint32_t handle) const {
    return value_for(handle);
}

void HostNativeWindow::free_guest(std::uint32_t address) {
    if (address == 0) return;
    if (deallocate_) {
        deallocate_(address);
        return;
    }
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::uint32_t HostNativeWindow::allocate_guest(std::uint32_t bytes) {
    if (allocate_) return allocate_(bytes);
    GuestCall args;
    args.regs = {bytes, 0, 0, 0};
    const auto allocated = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    return allocated ? allocated->r0 : 0;
}

std::int32_t HostNativeWindow::lock_window(std::uint32_t handle, std::uint32_t out_buffer,
                                           std::uint32_t dirty_address) {
    const void* window = require_window(handle);
    if (window == nullptr) return -EINVAL;
    // ARM32 ANativeWindow_Buffer is five 32-bit fields followed by six reserved words.
    auto* out = runtime_.memory().host_ptr(out_buffer, 44, kPageWrite);
    if (out == nullptr) return -EFAULT;
    NativeWindowBackend::Rect dirty{};
    NativeWindowBackend::Rect* dirty_ptr = nullptr;
    if (dirty_address != 0) {
        auto* input = runtime_.memory().host_ptr(dirty_address, sizeof dirty, kPageRead | kPageWrite);
        if (input == nullptr) return -EFAULT;
        std::memcpy(&dirty, input, sizeof dirty);
        dirty_ptr = &dirty;
    }
    {
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        if (locks_.contains(handle)) return -EBUSY;
        locks_[handle] = {0, 0, nullptr, std::this_thread::get_id()};
    }
    NativeWindowBackend::Buffer buffer;
    const auto result = backend_.lock(const_cast<void*>(window), buffer, dirty_ptr);
    if (result != 0) {
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_.erase(handle);
        return result;
    }
    // Android pixel formats: RGBA_8888, RGBX_8888, RGB_888, RGB_565, RGBA_FP16,
    // and RGBA_1010102. Reject all other formats until their row layout is defined.
    const std::uint32_t pixel_bytes = buffer.format == 1 || buffer.format == 2 || buffer.format == 43 ? 4 :
                                      buffer.format == 3 ? 3 : buffer.format == 4 ? 2 :
                                      buffer.format == 22 ? 8 : 0;
    const std::uint64_t row_bytes = buffer.stride > 0 ? static_cast<std::uint64_t>(buffer.stride) * pixel_bytes : 0;
    const bool valid = buffer.bits != nullptr && buffer.width > 0 && buffer.height > 0 &&
                       buffer.stride >= buffer.width && pixel_bytes != 0 && row_bytes != 0 &&
                       row_bytes <= (64ull << 20) &&
                       static_cast<std::uint64_t>(buffer.height) <= (64ull << 20) / row_bytes;
    if (!valid) {
        (void)backend_.unlock_and_post(const_cast<void*>(window));
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_.erase(handle);
        return -ENOTSUP;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(buffer.height) * row_bytes;
    const auto allocated = allocate_guest(static_cast<std::uint32_t>(bytes));
    if (allocated == 0) {
        (void)backend_.unlock_and_post(const_cast<void*>(window));
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_.erase(handle);
        return -ENOMEM;
    }
    const auto overlaps = [&](std::uint32_t address, std::uint64_t length) {
        return address != 0 && static_cast<std::uint64_t>(address) <
            static_cast<std::uint64_t>(allocated) + bytes &&
            static_cast<std::uint64_t>(allocated) < static_cast<std::uint64_t>(address) + length;
    };
    if (overlaps(out_buffer, 44) || overlaps(dirty_address, sizeof dirty)) {
        free_guest(allocated);
        (void)backend_.unlock_and_post(const_cast<void*>(window));
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_.erase(handle);
        return -EINVAL;
    }
    auto* pixels = runtime_.memory().host_ptr(allocated, bytes, kPageRead | kPageWrite);
    if (pixels == nullptr) {
        free_guest(allocated);
        (void)backend_.unlock_and_post(const_cast<void*>(window));
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_.erase(handle);
        return -EFAULT;
    }
    std::memcpy(pixels, buffer.bits, static_cast<std::size_t>(bytes));
    std::uint32_t guest_buffer[11] = {
        static_cast<std::uint32_t>(buffer.width), static_cast<std::uint32_t>(buffer.height),
        static_cast<std::uint32_t>(buffer.stride), static_cast<std::uint32_t>(buffer.format),
        allocated, 0, 0, 0, 0, 0, 0};
    std::memcpy(out, guest_buffer, sizeof guest_buffer);
    if (dirty_ptr != nullptr) {
        auto* guest_dirty = runtime_.memory().host_ptr(dirty_address, sizeof dirty, kPageWrite);
        std::memcpy(guest_dirty, &dirty, sizeof dirty);
    }
    {
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        locks_[handle] = {allocated, bytes, buffer.bits, std::this_thread::get_id()};
    }
    return 0;
}

std::int32_t HostNativeWindow::unlock_window(std::uint32_t handle) {
    const void* window = require_window(handle);
    if (window == nullptr) return -EINVAL;
    LockState state{};
    {
        std::lock_guard<std::mutex> guard(surfaces_mutex_);
        const auto it = locks_.find(handle);
        if (it == locks_.end()) return -EINVAL;
        if (it->second.address == 0) return -EBUSY;
        if (it->second.owner != std::this_thread::get_id()) return -EPERM;
        state = it->second;
        locks_.erase(it);
    }
    const auto* pixels = runtime_.memory().host_ptr(state.address, state.bytes, kPageRead);
    if (pixels != nullptr) std::memcpy(state.bits, pixels, static_cast<std::size_t>(state.bytes));
    free_guest(state.address);
    const auto result = backend_.unlock_and_post(const_cast<void*>(window));
    return pixels == nullptr ? -EFAULT : result;
}

bool HostNativeWindow::handle_host_call(std::uint32_t index, GuestThread& thread) {
    auto& regs = thread.regs();
    switch (index) {
    case ZB_WINDOW_HC_ANativeWindow_fromSurface: {
        // regs[0] is the guest JNIEnv*: ignored, we use the real host JNIEnv of this thread.
        const JniBackend::Ref surface = host_jni_.resolve_ref(regs[1], "ANativeWindow_fromSurface");
        void* window = nullptr;
        if (surface != 0) {
            void* env = as_pointer(static_cast<std::uint64_t>(host_jni_.current_env()));
            window = backend_.from_surface(env, as_pointer(surface));
        }
        std::uint32_t handle = 0;
        if (window != nullptr) {
            handle = windows_.add(from_pointer(window));
            if (handle == 0) {
                backend_.release(window);
            } else {
                std::lock_guard<std::mutex> lock(surfaces_mutex_);
                references_[handle] = 1;
            }
        }
        regs[0] = handle;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_acquire: {
        const void* window = require_window(regs[0]);
        if (window != nullptr) {
            std::lock_guard<std::mutex> lock(surfaces_mutex_);
            auto it = references_.find(regs[0]);
            if (it != references_.end() && it->second != UINT32_MAX) {
                backend_.acquire(const_cast<void*>(window));
                ++it->second;
            }
        }
        regs[0] = 0;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_release: {
        const void* window = require_window(regs[0]);
        if (window != nullptr) {
            bool final = false;
            bool locked = false;
            {
                std::lock_guard<std::mutex> lock(surfaces_mutex_);
                locked = locks_.contains(regs[0]);
                if (!locked) {
                    auto it = references_.find(regs[0]);
                    if (it != references_.end() && --it->second == 0) {
                        references_.erase(it);
                        final = true;
                    }
                }
            }
            if (locked) {
                log("ANativeWindow_release while locked: unlockAndPost is required first");
            } else {
                backend_.release(const_cast<void*>(window));
                if (final) (void)windows_.remove(regs[0]);
            }
        }
        regs[0] = 0;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getWidth: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Width) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getHeight: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Height) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getFormat: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Format) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_setBuffersGeometry: {
        const void* window = require_window(regs[0]);
        std::int32_t result = -1;
        if (window != nullptr) {
            result = backend_.set_buffers_geometry(const_cast<void*>(window), static_cast<std::int32_t>(regs[1]),
                                                   static_cast<std::int32_t>(regs[2]),
                                                   static_cast<std::int32_t>(regs[3]));
        }
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_toSurface: {
        const void* window = require_window(regs[1]);
        void* env = as_pointer(static_cast<std::uint64_t>(host_jni_.current_env()));
        void* surface = window == nullptr ? nullptr :
            backend_.to_surface(env, const_cast<void*>(window));
        regs[0] = host_jni_.new_local_handle(from_pointer(surface));
        return true;
    }
    case ZB_COMPAT_HC_ANativeWindow_lock:
        regs[0] = static_cast<std::uint32_t>(lock_window(regs[0], regs[1], regs[2]));
        regs[1] = 0;
        return true;
    case ZB_COMPAT_HC_ANativeWindow_unlockAndPost:
        regs[0] = static_cast<std::uint32_t>(unlock_window(regs[0]));
        regs[1] = 0;
        return true;
    default:
        return false;
    }
}

}  // namespace zb
