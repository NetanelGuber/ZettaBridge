#include <android/log.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <jni.h>
#include <poll.h>

#include <bit>
#include <cstdint>
#include <thread>

#include "../../core/android/input_driver_backend.h"

namespace {
zb::AndroidInputBackend backend;
constexpr const char* kTag = "ZB_STEP11_INPUT";
}

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity*, void*, size_t) {}

extern "C" JNIEXPORT void JNICALL
Java_com_zettabridge_step11inputfixture_InputActivity_onMotion(JNIEnv* env, jclass,
                                                                jobject java_event) {
    const AInputEvent* event = AMotionEvent_fromJava(env, java_event);
    std::uint64_t type = 0, pointers = 0, xbits = 0, ybits = 0;
    const bool queried = event != nullptr &&
        backend.query("AInputEvent_getType", reinterpret_cast<std::uintptr_t>(event),
                      0, 0, 0, 'i', type) &&
        backend.query("AMotionEvent_getPointerCount", reinterpret_cast<std::uintptr_t>(event),
                      0, 0, 0, 'i', pointers) &&
        backend.query("AMotionEvent_getX", reinterpret_cast<std::uintptr_t>(event),
                      0, 0, 0, 'f', xbits) &&
        backend.query("AMotionEvent_getY", reinterpret_cast<std::uintptr_t>(event),
                      0, 0, 0, 'f', ybits);
    __android_log_print(queried ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "synthetic motion type=%llu pointers=%llu x=%.1f y=%.1f queried=%d",
                        static_cast<unsigned long long>(type),
                        static_cast<unsigned long long>(pointers),
                        std::bit_cast<float>(static_cast<std::uint32_t>(xbits)),
                        std::bit_cast<float>(static_cast<std::uint32_t>(ybits)), queried ? 1 : 0);
    if (event != nullptr) AInputEvent_release(event);
}

extern "C" JNIEXPORT void JNICALL
Java_com_zettabridge_step11inputfixture_InputActivity_onQueue(JNIEnv* env, jclass, jobject queue) {
    const auto native = backend.from_java(reinterpret_cast<std::uintptr_t>(env),
                                          reinterpret_cast<std::uintptr_t>(queue));
    const int fd = backend.readiness_fd(native);
    __android_log_print(ANDROID_LOG_INFO, kTag, "queue=%s fd=%d", native ? "ready" : "failed", fd);
    if (native == 0 || fd < 0) return;
    std::thread([native, fd] {
        for (int sequence = 0; sequence < 4; ++sequence) {
            pollfd pfd{fd, POLLIN, 0};
            const int ready = poll(&pfd, 1, 30000);
            if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
                __android_log_print(ANDROID_LOG_ERROR, kTag, "no input event %d: poll=%d",
                                    sequence, ready);
                return;
            }
            std::uint64_t event = 0;
            const int result = backend.get_event(native, event);
            std::uint64_t type = 0, code = 0, action = 0;
            bool queried = result == 0 &&
                backend.query("AInputEvent_getType", event, 0, 0, 0, 'i', type);
            std::uint64_t pointers = 0, xbits = 0, ybits = 0;
            if (queried && type == 1) {
                queried = backend.query("AKeyEvent_getKeyCode", event, 0, 0, 0, 'i', code) &&
                    backend.query("AKeyEvent_getAction", event, 0, 0, 0, 'i', action);
            } else if (queried && type == 2) {
                queried = backend.query("AMotionEvent_getAction", event, 0, 0, 0, 'i', action) &&
                    backend.query("AMotionEvent_getPointerCount", event, 0, 0, 0, 'i', pointers) &&
                    backend.query("AMotionEvent_getX", event, 0, 0, 0, 'f', xbits) &&
                    backend.query("AMotionEvent_getY", event, 0, 0, 0, 'f', ybits);
            }
            if (result == 0) backend.finish_event(native, event, 1);
            __android_log_print(queried ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                                "event %d result=%d type=%llu key=%llu action=%llu pointers=%llu x=%.1f y=%.1f queried=%d",
                                sequence, result, static_cast<unsigned long long>(type),
                                static_cast<unsigned long long>(code),
                                static_cast<unsigned long long>(action),
                                static_cast<unsigned long long>(pointers),
                                std::bit_cast<float>(static_cast<std::uint32_t>(xbits)),
                                std::bit_cast<float>(static_cast<std::uint32_t>(ybits)),
                                queried ? 1 : 0);
        }
    }).detach();
}
