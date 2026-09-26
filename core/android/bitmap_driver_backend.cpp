#include "bitmap_driver_backend.h"

#include <android/bitmap.h>
#include <jni.h>

#include <cstdint>

namespace zb {
namespace {
JNIEnv* E(JniBackend::Env env) { return reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(env)); }
jobject O(JniBackend::Ref ref) { return reinterpret_cast<jobject>(static_cast<std::uintptr_t>(ref)); }
}  // namespace

int AndroidBitmapBackend::get_info(JniBackend::Env env, JniBackend::Ref bitmap, BitmapInfo& info) {
    AndroidBitmapInfo native{};
    const int result = AndroidBitmap_getInfo(E(env), O(bitmap), &native);
    if (result == 0) info = {native.width, native.height, native.stride, native.format, native.flags};
    return result;
}

int AndroidBitmapBackend::lock_pixels(JniBackend::Env env, JniBackend::Ref bitmap, void*& pixels) {
    return AndroidBitmap_lockPixels(E(env), O(bitmap), &pixels);
}

int AndroidBitmapBackend::unlock_pixels(JniBackend::Env env, JniBackend::Ref bitmap) {
    return AndroidBitmap_unlockPixels(E(env), O(bitmap));
}

JniBackend::Ref AndroidBitmapBackend::retain(JniBackend::Env env, JniBackend::Ref bitmap) {
    return reinterpret_cast<JniBackend::Ref>(E(env)->NewGlobalRef(O(bitmap)));
}

void AndroidBitmapBackend::release(JniBackend::Env env, JniBackend::Ref bitmap) {
    E(env)->DeleteGlobalRef(O(bitmap));
}

bool AndroidBitmapBackend::same_object(JniBackend::Env env, JniBackend::Ref a, JniBackend::Ref b) {
    return E(env)->IsSameObject(O(a), O(b)) == JNI_TRUE;
}

}  // namespace zb
