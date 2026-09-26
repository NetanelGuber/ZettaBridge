#include "input_driver_backend.h"

#include <android/input.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <thread>


namespace zb {
namespace {
std::uint64_t H(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
AInputEvent* E(std::uint64_t p) { return reinterpret_cast<AInputEvent*>(static_cast<std::uintptr_t>(p)); }
void drain(int fd) {
    std::uint64_t value;
    while (read(fd, &value, sizeof value) < 0 && errno == EINTR) {}
}
}  // namespace

AndroidInputBackend::QueueState* AndroidInputBackend::state(std::uint64_t queue) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = queues_.find(queue);
    return it == queues_.end() ? nullptr : it->second.get();
}

bool AndroidInputBackend::from_java_available() {
    return dlsym(RTLD_DEFAULT, "AInputQueue_fromJava") != nullptr;
}

std::uint64_t AndroidInputBackend::from_java(JniBackend::Env env, JniBackend::Ref queue) {
    auto* e = reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(env));
    auto object = reinterpret_cast<jobject>(static_cast<std::uintptr_t>(queue));
    if (e == nullptr || object == nullptr) return 0;
    jclass queue_type = e->FindClass("android/view/InputQueue");
    if (queue_type == nullptr) {
        e->ExceptionClear();
        return 0;
    }
    const bool valid_type = e->IsInstanceOf(object, queue_type) == JNI_TRUE;
    e->DeleteLocalRef(queue_type);
    if (!valid_type) return 0;
    // AInputQueue_fromJava holds only a weak reference. Retain its Java owner for the lifetime
    // of this process; the queue API has no matching release operation.
    jobject owner = e->NewGlobalRef(object);
    if (owner == nullptr) return 0;
    // API 33 export: the runtime still targets API 29, so resolve it at runtime.
    auto* from_java = reinterpret_cast<AInputQueue* (*)(JNIEnv*, jobject)>(
        dlsym(RTLD_DEFAULT, "AInputQueue_fromJava"));
    AInputQueue* native = from_java == nullptr ? nullptr : from_java(e, owner);
    if (native == nullptr) {
        e->DeleteGlobalRef(owner);
        return 0;
    }
    const auto key = H(native);
    std::lock_guard<std::mutex> lock(mutex_);
    if (queues_.contains(key)) {
        e->DeleteGlobalRef(owner);
        return key;
    }
    if (queues_.size() >= 64) {
        e->DeleteGlobalRef(owner);
        return 0;
    }
    auto state = std::make_unique<QueueState>();
    state->queue = native;
    state->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state->fd < 0) {
        e->DeleteGlobalRef(owner);
        return 0;
    }
    QueueState* raw = state.get();
    queues_.emplace(key, std::move(state));
    // Both the global ref and worker live until process exit, matching the NDK queue lifetime.
    (void)owner;
    std::thread(&AndroidInputBackend::run, raw).detach();
    return key;
}

int AndroidInputBackend::readiness_fd(std::uint64_t queue) {
    const auto* s = state(queue);
    return s == nullptr ? -1 : s->fd;
}

int AndroidInputBackend::has_events(std::uint64_t queue) {
    const auto* s = state(queue);
    return s == nullptr ? -EINVAL : AInputQueue_hasEvents(s->queue);
}

void AndroidInputBackend::rearm(QueueState& s) {
    if (AInputQueue_hasEvents(s.queue) != 0) return;
    drain(s.fd);
    s.ready.store(false, std::memory_order_release);
    if (auto* looper = s.looper.load(std::memory_order_acquire)) ALooper_wake(looper);
}

int AndroidInputBackend::get_event(std::uint64_t queue, std::uint64_t& event) {
    auto* s = state(queue);
    if (s == nullptr) return -EINVAL;
    AInputEvent* native = nullptr;
    const int result = AInputQueue_getEvent(s->queue, &native);
    if (result == 0) event = H(native);
    rearm(*s);
    return result;
}

int AndroidInputBackend::pre_dispatch(std::uint64_t queue, std::uint64_t event) {
    auto* s = state(queue);
    if (s == nullptr || event == 0) return -EINVAL;
    const int result = AInputQueue_preDispatchEvent(s->queue, E(event));
    if (result != 0) rearm(*s);
    return result;
}

void AndroidInputBackend::finish_event(std::uint64_t queue, std::uint64_t event, int handled) {
    auto* s = state(queue);
    if (s == nullptr || event == 0) return;
    AInputQueue_finishEvent(s->queue, E(event), handled);
    rearm(*s);
}

int AndroidInputBackend::on_ready(int, int, void* data) {
    auto& s = *static_cast<QueueState*>(data);
    if (!s.ready.exchange(true, std::memory_order_acq_rel)) {
        const std::uint64_t one = 1;
        (void)write(s.fd, &one, sizeof one);
    }
    return 1;
}

void AndroidInputBackend::run(QueueState* s) {
    ALooper* looper = ALooper_prepare(0);
    s->looper.store(looper, std::memory_order_release);
    if (looper == nullptr) return;
    bool attached = false;
    for (;;) {
        if (!s->ready.load(std::memory_order_acquire) && !attached) {
            AInputQueue_attachLooper(s->queue, looper, ALOOPER_POLL_CALLBACK,
                                     &AndroidInputBackend::on_ready, s);
            attached = true;
        }
        (void)ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        if (s->ready.load(std::memory_order_acquire) && attached) {
            AInputQueue_detachLooper(s->queue);
            attached = false;
        }
    }
}

bool AndroidInputBackend::query(const char* name, std::uint64_t event, std::uint32_t a,
                                std::uint32_t b, std::uint32_t c, char kind,
                                std::uint64_t& bits) {
    if (event == 0) return false;
    void* symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol == nullptr) return false;
    const auto n = std::string_view(name);
    const unsigned arity = n == "AMotionEvent_getHistoricalAxisValue" ? 3 :
        n == "AMotionEvent_getAxisValue" ||
        (n.starts_with("AMotionEvent_getHistorical") &&
         n != "AMotionEvent_getHistoricalEventTime") ? 2 :
        n == "AMotionEvent_getPointerId" || n == "AMotionEvent_getToolType" ||
        n == "AMotionEvent_getHistoricalEventTime" ||
        (n.starts_with("AMotionEvent_get") &&
         (n.ends_with("X") || n.ends_with("Y") || n.ends_with("Pressure") ||
          n.ends_with("Size") || n.ends_with("Major") || n.ends_with("Minor") ||
          n.ends_with("Orientation")) && n != "AMotionEvent_getHistorySize" &&
         !n.ends_with("Offset") &&
         !n.ends_with("Precision")) ? 1 : 0;
    const AInputEvent* e = E(event);
    if (kind == 'f') {
        float value = 0;
        if (arity == 0) value = reinterpret_cast<float (*)(const AInputEvent*)>(symbol)(e);
        else if (arity == 1) value = reinterpret_cast<float (*)(const AInputEvent*, size_t)>(symbol)(e, a);
        else if (arity == 2) value = reinterpret_cast<float (*)(const AInputEvent*, size_t, size_t)>(symbol)(e, a, b);
        else value = reinterpret_cast<float (*)(const AInputEvent*, int32_t, size_t, size_t)>(symbol)(e, a, b, c);
        bits = std::bit_cast<std::uint32_t>(value);
    } else if (kind == 'l') {
        const auto fn0 = reinterpret_cast<int64_t (*)(const AInputEvent*)>(symbol);
        const auto fn1 = reinterpret_cast<int64_t (*)(const AInputEvent*, size_t)>(symbol);
        bits = static_cast<std::uint64_t>(arity == 0 ? fn0(e) : fn1(e, a));
    } else {
        const auto fn0 = reinterpret_cast<int32_t (*)(const AInputEvent*)>(symbol);
        const auto fn1 = reinterpret_cast<int32_t (*)(const AInputEvent*, size_t)>(symbol);
        bits = static_cast<std::uint32_t>(arity == 0 ? fn0(e) : fn1(e, a));
    }
    return true;
}

}  // namespace zb
