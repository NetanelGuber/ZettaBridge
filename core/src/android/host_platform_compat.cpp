#include "zb/host_platform_compat.h"

#include <cerrno>
#include <string_view>

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
};

struct HostCall { std::uint32_t index; const char* library; const char* function; };
constexpr HostCall kHostCalls[] = {
#include "../gen/hostcalls.inc"
};

}  // namespace

bool HostPlatformCompat::handle_host_call(std::uint32_t index, GuestThread& thread) {
    for (const Fallback& fallback : kFallbacks) {
        if (fallback.index != index) continue;
        runtime_report().note_unimplemented_host_call(index, fallback.library, fallback.function);
        thread.regs()[0] = static_cast<std::uint32_t>(fallback.result);
        thread.regs()[1] = 0;
        return true;
    }
    for (const auto& call : kHostCalls) {
        if (call.index != index || std::string_view(call.library) != "libandroid.so") continue;
        const std::string_view name(call.function);
        if (!name.starts_with("AInputQueue_") && !name.starts_with("AConfiguration_") &&
            !name.starts_with("AInputEvent_") && !name.starts_with("AKeyEvent_") &&
            !name.starts_with("AMotionEvent_")) break;
        runtime_report().note_unimplemented_host_call(index, call.library, call.function);
        // Pointer results are NULL; status results are -ENOSYS. Void calls have no status
        // channel, so their use is preserved in the runtime report.
        const bool pointer = name == "AInputQueue_fromJava" || name == "AConfiguration_new" ||
                             name == "AInputEvent_toJava" || name.ends_with("_fromJava");
        const bool status = name == "AInputQueue_getEvent" || name == "AInputQueue_hasEvents" ||
                            name == "AInputQueue_preDispatchEvent" ||
                            name.starts_with("AInputEvent_get") || name.starts_with("AKeyEvent_get") ||
                            name.starts_with("AMotionEvent_get") ||
                            name.starts_with("AConfiguration_get") || name == "AConfiguration_diff" ||
                            name == "AConfiguration_match" || name == "AConfiguration_isBetterThan";
        thread.regs()[0] = pointer ? 0 : static_cast<std::uint32_t>(status ? -ENOSYS : 0);
        thread.regs()[1] = 0;
        return true;
    }
    return false;
}

}  // namespace zb
