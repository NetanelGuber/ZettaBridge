#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>

// Each bit proves a concrete NDK call sequence in an installed converted APK.
JNIEXPORT jint JNICALL
Java_com_zettabridge_step11fixture_ProbeActivity_probe(JNIEnv* env, jclass cls,
                                                        jobject assets, jobject bitmap,
                                                        jobject surface) {
    (void)cls;
    uint32_t passed = 0;
    AAssetManager* manager = AAssetManager_fromJava(env, assets);
    if (manager) {
        AAsset* asset = AAssetManager_open(manager, "step11.txt", AASSET_MODE_BUFFER);
        if (asset) {
            char value[32] = {};
            if (AAsset_getLength(asset) == 16 && AAsset_read(asset, value, sizeof value) == 16 &&
                memcmp(value, "step11-asset-ok\n", 16) == 0) passed |= 1;
            AAsset_close(asset);
        }
    }
    AndroidBitmapInfo info = {};
    void* pixels = 0;
    if (AndroidBitmap_getInfo(env, bitmap, &info) == 0 && info.width == 2 &&
        info.height == 2 && info.format == ANDROID_BITMAP_FORMAT_RGBA_8888 &&
        AndroidBitmap_lockPixels(env, bitmap, &pixels) == 0) {
        ((uint32_t*)pixels)[0] = 0xff336699u;
        if (AndroidBitmap_unlockPixels(env, bitmap) == 0) passed |= 2;
    }
    AConfiguration* config = AConfiguration_new();
    if (config) {
        AConfiguration_fromAssetManager(config, manager);
        if (AConfiguration_getDensity(config) > 0) passed |= 4;
        AConfiguration_delete(config);
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) {
        ANativeWindow_Buffer buffer = {};
        if (ANativeWindow_getWidth(window) > 0 && ANativeWindow_getHeight(window) > 0 &&
            ANativeWindow_lock(window, &buffer, 0) == 0) {
            if (buffer.bits && buffer.width > 0 && buffer.height > 0 && buffer.stride >= buffer.width)
                passed |= 8;
            if (ANativeWindow_unlockAndPost(window) == 0) passed |= 16;
        }
        ANativeWindow_release(window);
    }
    return (jint)passed;
}
