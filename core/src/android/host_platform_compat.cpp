#include "zb/host_platform_compat.h"

#include <cerrno>
#include <limits>

#include "zb/platform_compat_hostcalls.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

struct Fallback {
    std::uint32_t index;
    const char* library;
    const char* function;
    std::int32_t result;
};

constexpr Fallback kFallbacks[] = {
    {ZB_COMPAT_HC_ANativeWindow_lock, "libandroid.so", "ANativeWindow_lock", -ENOSYS},
    {ZB_COMPAT_HC_ANativeWindow_unlockAndPost, "libandroid.so",
     "ANativeWindow_unlockAndPost", -ENOSYS},
    {ZB_COMPAT_HC_eglCreateImageKHR, "libEGL.so", "eglCreateImageKHR", 0},
    {ZB_COMPAT_HC_eglDestroyImageKHR, "libEGL.so", "eglDestroyImageKHR", 0},
    {ZB_COMPAT_HC_glEGLImageTargetTexture2DOES, "libGLESv2.so",
     "glEGLImageTargetTexture2DOES", 0},
    {ZB_COMPAT_HC_AndroidBitmap_getInfo, "libjnigraphics.so", "AndroidBitmap_getInfo", -1},
    {ZB_COMPAT_HC_AndroidBitmap_lockPixels, "libjnigraphics.so", "AndroidBitmap_lockPixels", -1},
    {ZB_COMPAT_HC_AndroidBitmap_unlockPixels, "libjnigraphics.so", "AndroidBitmap_unlockPixels", -1},
    {ZB_COMPAT_HC_ALooper_addFd, "libandroid.so", "ALooper_addFd", -1},
    {ZB_COMPAT_HC_ALooper_pollOnce, "libandroid.so", "ALooper_pollOnce", -4},
    {ZB_COMPAT_HC_ALooper_removeFd, "libandroid.so", "ALooper_removeFd", 0},
    {ZB_COMPAT_HC_ALooper_wake, "libandroid.so", "ALooper_wake", 0},
};

}  // namespace

std::uint32_t HostPlatformCompat::looper_for_thread(GuestThread& thread, bool prepare) {
    std::lock_guard<std::mutex> lock(looper_mutex_);
    const auto existing = thread_loopers_.find(&thread);
    if (existing != thread_loopers_.end()) return existing->second;
    if (!prepare) return 0;

    const std::uint32_t handle = next_looper_;
    next_looper_ += 4;
    thread_loopers_.emplace(&thread, handle);
    looper_refs_.emplace(handle, 1);
    return handle;
}

void HostPlatformCompat::acquire_looper(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(looper_mutex_);
    const auto it = looper_refs_.find(handle);
    if (it != looper_refs_.end() && it->second != std::numeric_limits<std::uint32_t>::max()) {
        ++it->second;
    }
}

void HostPlatformCompat::release_looper(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(looper_mutex_);
    const auto it = looper_refs_.find(handle);
    if (it != looper_refs_.end() && it->second > 1) --it->second;
}

bool HostPlatformCompat::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index == ZB_COMPAT_HC_ALooper_forThread || index == ZB_COMPAT_HC_ALooper_prepare) {
        thread.regs()[0] = looper_for_thread(thread, index == ZB_COMPAT_HC_ALooper_prepare);
        thread.regs()[1] = 0;
        return true;
    }
    if (index == ZB_COMPAT_HC_ALooper_acquire || index == ZB_COMPAT_HC_ALooper_release) {
        const std::uint32_t handle = thread.regs()[0];
        if (index == ZB_COMPAT_HC_ALooper_acquire) {
            acquire_looper(handle);
        } else {
            release_looper(handle);
        }
        thread.regs()[0] = 0;
        thread.regs()[1] = 0;
        return true;
    }
    for (const Fallback& fallback : kFallbacks) {
        if (fallback.index != index) continue;
        runtime_report().note_unimplemented_host_call(index, fallback.library, fallback.function);
        thread.regs()[0] = static_cast<std::uint32_t>(fallback.result);
        thread.regs()[1] = 0;
        return true;
    }
    return false;
}

}  // namespace zb
