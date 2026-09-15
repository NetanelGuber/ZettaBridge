#pragma once

// Shared state of HostJni. host_jni.cpp holds registration, per-thread state, handle and id
// resolution and native calls; the flat host calls are served in groups, one source file per group
// (core/src/jni/host_jni_<group>.cpp).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "zb/host_jni.h"
#include "zb/jni_handles.h"
#include "zb/jni_hostcalls.h"
#include "zb/jni_protocol.h"

namespace zb {

// JNI state of one host thread.
struct JniThread {
    HostJni::Impl* owner = nullptr;
    JniBackend::Env env = 0;      // host JNIEnv of this thread while it has one
    std::uint32_t guest_env = 0;  // guest JNIEnv*, allocated on first use
    int native_depth = 0;         // open call_native frames
    int user_frames = 0;          // guest PushLocalFrame frames of the innermost native call
    LocalHandles locals;
    std::unique_ptr<LibraryRuntime::Carrier> carrier;

    ~JniThread();
};

// One flat JNI host call: arguments per AAPCS32 (r0-r3, then the stack), result in r0/r1.
class JniCall {
public:
    // Captures r0-r3 and clears r0/r1, the result of a call that sets nothing.
    JniCall(HostJni::Impl& jni, GuestThread& thread, JniThread& state, std::uint32_t index)
        : jni_(jni), thread_(thread), state_(state), index_(index),
          regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
        thread.regs()[0] = 0;
        thread.regs()[1] = 0;
    }

    std::uint32_t index() const { return index_; }
    std::uint32_t arg(unsigned position) const;
    void set(std::uint32_t value) { thread_.regs()[0] = value; }
    void set64(std::uint64_t value) {
        thread_.regs()[0] = static_cast<std::uint32_t>(value);
        thread_.regs()[1] = static_cast<std::uint32_t>(value >> 32);
    }
    JniThread& state() { return state_; }
    // The host JNIEnv of the calling thread; a thread without one is a fatal error.
    JniBackend::Env env();

private:
    HostJni::Impl& jni_;
    GuestThread& thread_;
    JniThread& state_;
    std::uint32_t index_;
    std::uint32_t regs_[4];
};

struct HostJni::Impl {
    Impl(LibraryRuntime& runtime, JniBackend& backend, std::size_t slot_capacity)
        : runtime(runtime), backend(backend), slots(slot_capacity) {}

    HostJni* owner = nullptr;
    LibraryRuntime& runtime;
    JniBackend& backend;
    std::atomic<bool> ready{false};
    zb_jni_guest_api api{};

    GlobalHandles globals{HandleKind::Global};
    GlobalHandles weaks{HandleKind::WeakGlobal};
    IdTable methods;
    IdTable fields;
    std::mutex shorty_mutex;
    std::vector<std::string> shorties;  // index: guest method id - 1
    NativeSlots slots;
    std::atomic<bool> logged_foreign_buffer{false};

    // The JniThread of the calling host thread.
    JniThread& thread();
    // Logs, reports through the backend's FatalError (when the thread has a JNIEnv) and aborts.
    [[noreturn]] void fatal(JniBackend::Env env, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    // Host reference of a handle of any kind (0 for null); an invalid handle is fatal.
    JniBackend::Ref resolve(JniThread& state, std::uint32_t handle, const char* function);
    // A host local reference as a handle of the calling thread's top frame.
    std::uint32_t local(JniThread& state, JniBackend::Ref ref) { return state.locals.add(ref); }

    // Guest method ids and their shorties.
    std::uint32_t intern_method(JniBackend::Id id, const std::string& shorty);
    // The shorty of a guest method id; nullopt for an unknown id.
    std::optional<std::string> method_shorty(std::uint32_t id);
    JniBackend::Id method_id(JniBackend::Env env, std::uint32_t id, const char* function);
    JniBackend::Id field_id(JniBackend::Env env, std::uint32_t id, const char* function);

    // Guest memory. Unreadable or unwritable ranges are fatal.
    std::string read_string(JniBackend::Env env, std::uint32_t address, const char* function);
    const std::uint8_t* readable(JniBackend::Env env, std::uint32_t address, std::uint64_t size, const char* function);
    std::uint8_t* writable(JniBackend::Env env, std::uint32_t address, std::uint64_t size, const char* function);
    // Writes a shorty into a guest ZB_JNI_SHORTY_SIZE buffer.
    void write_shorty(JniBackend::Env env, std::uint32_t address, const std::string& shorty, const char* function);

    // Runs a guest function on the guest thread this host thread runs, or on its carrier.
    std::optional<GuestResult> invoke(JniThread& state, std::uint32_t function, const GuestCall& args);
    // Allocates the guest JNIEnv of this thread if needed; false if the guest allocation failed.
    bool ensure_guest_env(JniThread& state);

    // Host-call groups; each returns false for indices it does not serve.
    bool serve_objects(JniCall& call);
    bool serve_values(JniCall& call);
    bool serve_data(JniCall& call);
    bool serve_natives(JniCall& call);
};

const char* jni_host_call_name(std::uint32_t index);
// Makes jni the target of the process-wide native thunk dispatcher (host_jni_natives.cpp).
void install_native_dispatcher(HostJni::Impl* jni);

}  // namespace zb
