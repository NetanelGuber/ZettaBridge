#include "zb/host_input.h"

#include <cerrno>
#include <bit>
#include <cstring>
#include <limits>
#include <string_view>

#include "zb/guest_memory.h"
#include "zb/host_jni.h"
#include "zb/host_looper.h"
#include "zb/runtime_report.h"

namespace zb {
namespace {
struct Call { std::uint32_t index; const char* library; const char* name; };
constexpr Call kCalls[] = {
#include "../gen/hostcalls.inc"
};

const char* input_name(std::uint32_t index) {
    for (const auto& call : kCalls) {
        if (call.index != index || std::string_view(call.library) != "libandroid.so") continue;
        const std::string_view name(call.name);
        if (name.starts_with("AInputQueue_") || name.starts_with("AInputEvent_") ||
            name.starts_with("AKeyEvent_") || name.starts_with("AMotionEvent_")) return call.name;
    }
    return nullptr;
}

char result_kind(std::string_view name) {
    if (name == "AKeyEvent_getDownTime" || name == "AKeyEvent_getEventTime" ||
        name == "AMotionEvent_getDownTime" || name == "AMotionEvent_getEventTime" ||
        name == "AMotionEvent_getHistoricalEventTime") return 'l';
    if (name.starts_with("AMotionEvent_get") &&
        name != "AMotionEvent_getAction" && name != "AMotionEvent_getFlags" &&
        name != "AMotionEvent_getMetaState" && name != "AMotionEvent_getButtonState" &&
        name != "AMotionEvent_getEdgeFlags" && name != "AMotionEvent_getPointerCount" &&
        name != "AMotionEvent_getPointerId" && name != "AMotionEvent_getToolType" &&
        name != "AMotionEvent_getHistorySize" && name != "AMotionEvent_getActionButton" &&
        name != "AMotionEvent_getClassification") return 'f';
    return 'i';
}
}  // namespace

bool HostInput::handle_host_call(std::uint32_t index, GuestThread& thread) {
    const char* raw_name = input_name(index);
    if (raw_name == nullptr) return false;
    const std::string_view name(raw_name);
    auto& r = thread.regs();
    const std::uint32_t a = r[0], b = r[1], c = r[2], d = r[3];
    const auto finish = [&](std::int32_t value, std::uint32_t high = 0) {
        r[0] = static_cast<std::uint32_t>(value);
        r[1] = high;
        return true;
    };
    const auto unsupported = [&]() {
        runtime_report().note_unimplemented_host_call(index, "libandroid.so", raw_name);
        const bool pointer = name == "AInputEvent_toJava" || name.ends_with("_fromJava");
        return finish(pointer ? 0 : -ENOSYS);
    };
    std::lock_guard<std::mutex> lock(mutex_);

    if (name == "AInputQueue_fromJava") {
        if (!backend_.from_java_available()) return unsupported();
        const auto context = context_ ? context_(b) :
            std::pair{jni_.current_env(), b == 0 ? JniBackend::Ref{0} : jni_.resolve_ref(b, raw_name)};
        const auto [env, object] = context;
        if (env == 0 || b == 0) return finish(0);
        if (object == 0) return finish(0);
        const std::uint64_t native = backend_.from_java(env, object);
        if (native == 0) return finish(0);
        for (const auto& [handle, queue] : queue_state_) {
            if (queue.native == native) return finish(static_cast<std::int32_t>(handle));
        }
        const int fd = backend_.readiness_fd(native);
        if (fd < 0) return finish(0);
        const std::uint32_t handle = queues_.add(native);
        if (handle == 0) return finish(0);
        queue_state_.emplace(handle, Queue{native, 0, fd});
        return finish(static_cast<std::int32_t>(handle));
    }

    if (name.starts_with("AInputQueue_")) {
        auto qi = queue_state_.find(a);
        if (qi == queue_state_.end() || queues_.get(a) != qi->second.native)
            return finish(-EINVAL);
        Queue& queue = qi->second;
        if (name == "AInputQueue_attachLooper") {
            const auto* source = runtime_.memory().host_ptr(r[13], 4, kPageRead);
            if (source == nullptr) return finish(-EFAULT);
            std::uint32_t data;
            std::memcpy(&data, source, 4);
            if (b == 0) return finish(-EINVAL);
            if (looper_.add_external_fd(b, queue.fd, static_cast<std::int32_t>(c), d, data) != 1)
                return finish(-EINVAL);
            if (queue.looper != 0 && queue.looper != b)
                (void)looper_.remove_external_fd(queue.looper, queue.fd);
            queue.looper = b;
            return finish(0);
        }
        if (name == "AInputQueue_detachLooper") {
            if (queue.looper != 0) {
                (void)looper_.remove_external_fd(queue.looper, queue.fd);
                queue.looper = 0;
            }
            return finish(0);
        }
        if (name == "AInputQueue_hasEvents") return finish(backend_.has_events(queue.native));
        if (name == "AInputQueue_getEvent") {
            auto* out = runtime_.memory().host_ptr(b, 4, kPageWrite);
            if (out == nullptr) return finish(-EFAULT);
            std::uint64_t native_event = 0;
            const int result = backend_.get_event(queue.native, native_event);
            if (result != 0) return finish(result);
            if (native_event == 0) return finish(-EIO);
            const std::uint32_t handle = events_.add(native_event);
            if (handle == 0) {
                backend_.finish_event(queue.native, native_event, 0);
                return finish(-ENOMEM);
            }
            event_state_.emplace(handle, Event{native_event, a, std::this_thread::get_id()});
            std::memcpy(out, &handle, 4);
            return finish(0);
        }
        auto ei = event_state_.find(b);
        if (ei == event_state_.end() || ei->second.queue != a ||
            ei->second.owner != std::this_thread::get_id()) return finish(-EINVAL);
        if (name == "AInputQueue_preDispatchEvent") {
            const int result = backend_.pre_dispatch(queue.native, ei->second.native);
            if (result != 0) {
                (void)events_.remove(b);
                event_state_.erase(ei);
            }
            return finish(result);
        }
        if (name == "AInputQueue_finishEvent") {
            backend_.finish_event(queue.native, ei->second.native, static_cast<int>(c));
            (void)events_.remove(b);
            event_state_.erase(ei);
            return finish(0);
        }
        return unsupported();
    }

    // These require a distinct lifetime/JNI contract and are deliberately reported until used.
    if (name == "AInputEvent_release" || name == "AInputEvent_toJava" ||
        name == "AKeyEvent_fromJava" || name == "AMotionEvent_fromJava") return unsupported();
    const auto ei = event_state_.find(a);
    if (ei == event_state_.end() || events_.get(a) != ei->second.native ||
        ei->second.owner != std::this_thread::get_id()) return finish(-EINVAL);
    const int event_type = name.starts_with("AKeyEvent_") ? 1 :
                           name.starts_with("AMotionEvent_") ? 2 : 0;
    if (event_type != 0) {
        std::uint64_t type = 0;
        if (!backend_.query("AInputEvent_getType", ei->second.native, 0, 0, 0, 'i', type))
            return unsupported();
        if (static_cast<int>(type) != event_type) return finish(-EINVAL);
    }
    const char kind = result_kind(name);
    const auto invalid_index = [&]() {
        if (kind == 'f') return finish(static_cast<std::int32_t>(
            std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN())));
        if (kind == 'l') return finish(-EINVAL, UINT32_MAX);
        return finish(-EINVAL);
    };
    if (name.starts_with("AMotionEvent_get")) {
        const bool historical = name.starts_with("AMotionEvent_getHistorical");
        const bool axis = name == "AMotionEvent_getAxisValue" ||
                          name == "AMotionEvent_getHistoricalAxisValue";
        const bool pointer = axis || name == "AMotionEvent_getPointerId" ||
            name == "AMotionEvent_getToolType" || name == "AMotionEvent_getRawX" ||
            name == "AMotionEvent_getRawY" || name == "AMotionEvent_getX" ||
            name == "AMotionEvent_getY" || name == "AMotionEvent_getPressure" ||
            name == "AMotionEvent_getSize" || name == "AMotionEvent_getTouchMajor" ||
            name == "AMotionEvent_getTouchMinor" || name == "AMotionEvent_getToolMajor" ||
            name == "AMotionEvent_getToolMinor" || name == "AMotionEvent_getOrientation" ||
            (historical && name != "AMotionEvent_getHistoricalEventTime" &&
             name != "AMotionEvent_getHistorySize");
        std::uint64_t count = 0;
        if (pointer) {
            if (!backend_.query("AMotionEvent_getPointerCount", ei->second.native, 0, 0, 0,
                                'i', count)) return unsupported();
            const std::uint32_t index_arg = axis ? c : b;
            if (index_arg >= count) return invalid_index();
        }
        if (historical) {
            if (!backend_.query("AMotionEvent_getHistorySize", ei->second.native, 0, 0, 0,
                                'i', count)) return unsupported();
            const std::uint32_t index_arg = name == "AMotionEvent_getHistoricalEventTime" ? b :
                                            axis ? d : c;
            if (index_arg >= count) return invalid_index();
        }
    }
    std::uint64_t bits = 0;
    if (!backend_.query(raw_name, ei->second.native, b, c, d, kind, bits)) return unsupported();
    return finish(static_cast<std::int32_t>(bits), static_cast<std::uint32_t>(bits >> 32));
}

}  // namespace zb
