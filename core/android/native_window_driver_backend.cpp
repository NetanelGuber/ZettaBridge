#include "native_window_driver_backend.h"

#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

namespace zb {

void* AndroidNativeWindowBackend::from_surface(void* env, void* surface) {
    return ANativeWindow_fromSurface(static_cast<JNIEnv*>(env), static_cast<jobject>(surface));
}

void* AndroidNativeWindowBackend::to_surface(void* env, void* window) {
    if (env == nullptr || window == nullptr) return nullptr;
    return ANativeWindow_toSurface(static_cast<JNIEnv*>(env), static_cast<ANativeWindow*>(window));
}

void AndroidNativeWindowBackend::acquire(void* window) {
    if (window == nullptr) return;
    ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
}

void AndroidNativeWindowBackend::release(void* window) {
    if (window == nullptr) return;
    ANativeWindow_release(static_cast<ANativeWindow*>(window));
}

std::int32_t AndroidNativeWindowBackend::query(void* window, Query which) {
    if (window == nullptr) return -1;
    auto* native = static_cast<ANativeWindow*>(window);
    switch (which) {
    case Query::Width: return ANativeWindow_getWidth(native);
    case Query::Height: return ANativeWindow_getHeight(native);
    case Query::Format: return ANativeWindow_getFormat(native);
    }
    return -1;
}

std::int32_t AndroidNativeWindowBackend::set_buffers_geometry(void* window, std::int32_t width,
                                                               std::int32_t height,
                                                               std::int32_t format) {
    if (window == nullptr) return -1;
    return ANativeWindow_setBuffersGeometry(static_cast<ANativeWindow*>(window), width, height,
                                            format);
}

std::int32_t AndroidNativeWindowBackend::lock(void* window, Buffer& buffer, Rect* dirty) {
    if (window == nullptr) return -1;
    ANativeWindow_Buffer native{};
    ARect bounds{};
    ARect* bounds_ptr = nullptr;
    if (dirty != nullptr) {
        bounds = {dirty->left, dirty->top, dirty->right, dirty->bottom};
        bounds_ptr = &bounds;
    }
    const auto result = ANativeWindow_lock(static_cast<ANativeWindow*>(window), &native, bounds_ptr);
    if (result != 0) return result;
    buffer = {native.width, native.height, native.stride, native.format, native.bits};
    if (dirty != nullptr) *dirty = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    return 0;
}

std::int32_t AndroidNativeWindowBackend::unlock_and_post(void* window) {
    return window == nullptr ? -1 : ANativeWindow_unlockAndPost(static_cast<ANativeWindow*>(window));
}

}  // namespace zb
