#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "zb/input_backend.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;
class HostLooper;

// Queue handles are process-lifetime because the NDK has no queue release API. Each getEvent
// makes a separate, one-use event handle; finishEvent invalidates it. Accessors never see host
// pointers and may only run on the thread that received the event.
class HostInput {
public:
    using Context = std::function<std::pair<JniBackend::Env, JniBackend::Ref>(std::uint32_t)>;
    HostInput(LibraryRuntime& runtime, HostJni& jni, HostLooper& looper, InputBackend& backend)
        : runtime_(runtime), jni_(jni), looper_(looper), backend_(backend) {}
    HostInput(LibraryRuntime& runtime, HostJni& jni, HostLooper& looper, InputBackend& backend,
              Context context)
        : runtime_(runtime), jni_(jni), looper_(looper), backend_(backend), context_(std::move(context)) {}
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    struct Queue { std::uint64_t native; std::uint32_t looper = 0; int fd = -1; };
    struct Event { std::uint64_t native; std::uint32_t queue; std::thread::id owner; };
    LibraryRuntime& runtime_;
    HostJni& jni_;
    HostLooper& looper_;
    InputBackend& backend_;
    Context context_;
    GlobalHandles queues_{HandleKind::Global};
    GlobalHandles events_{HandleKind::WeakGlobal};
    std::unordered_map<std::uint32_t, Queue> queue_state_;
    std::unordered_map<std::uint32_t, Event> event_state_;
    std::mutex mutex_;
};

}  // namespace zb
