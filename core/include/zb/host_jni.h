#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "zb/guest_thread.h"
#include "zb/jni_backend.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"
#include "zb/native_thunks.h"

namespace zb {

// The host side of the guest JNIEnv (guest/zbjni/zbjni.c -> libzbjni.so). It serves the flat JNI
// host calls against a JniBackend, owns the 32-bit handle and id tables and the thunk slots, and
// runs guest native code on behalf of Java callers.
//
// One instance per process: the constructor installs the process-wide native dispatcher. Like
// the LibraryRuntime it is process-lifetime and must outlive every guest thread.
class HostJni {
public:
    // Builds the arguments of a native call. guest_env is the guest JNIEnv* of the calling host
    // thread; to_handle turns a host reference into a local handle of the call's frame.
    using BuildCall = std::function<GuestCall(std::uint32_t guest_env, const RefToHandle& to_handle)>;

    struct NativeResult {
        GuestResult guest;
        // Return type 'L': the returned object as a host local reference in the caller's frame.
        JniBackend::Ref ref = 0;
    };

    HostJni(LibraryRuntime& runtime, JniBackend& backend, std::size_t slot_capacity = kNativeThunkCount);
    ~HostJni();
    HostJni(const HostJni&) = delete;
    HostJni& operator=(const HostJni&) = delete;

    // Serves JNI host calls (0xFB00-0xFCFF). Chain it into LibraryRuntime::set_host_call_handler
    // before start(); returns false for other indices.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    // True once libzbjni.so has registered its API (while zbhost preloads it).
    bool ready() const;
    // The guest JavaVM*, valid when ready().
    std::uint32_t guest_java_vm() const;

    // The host JNIEnv of the calling thread's current JNI transition (call_native or a guest JNI
    // host call reached from one), or 0 outside one. Lets another host-call dispatcher (HostAssets)
    // call back into Java on the same thread.
    JniBackend::Env current_env();
    // Resolves a guest JNI local/global/weak-global reference handle to a host reference on the
    // calling thread; 0 for the null handle. Fatal for an invalid handle, like every JNI reference
    // operation. function names the caller for the error message.
    JniBackend::Ref resolve_ref(std::uint32_t handle, const char* function);

    // Runs a guest function as native code called from Java on the calling host thread, whose
    // host JNIEnv is env. Uses the guest thread this host thread already runs, or else this host
    // thread's cached carrier. Opens a local frame for the call; return_type 'L' converts the
    // returned handle before the frame closes. nullopt if the frame could not be opened (a Java
    // exception is pending) or the guest call failed.
    std::optional<NativeResult> call_native(JniBackend::Env env, char return_type, std::uint32_t function,
                                            const BuildCall& build);

    // Binds one Java native method to a guest function through a thunk slot: strips one leading
    // '!' from the signature, allocates a slot, and calls the backend once. Returns 0, or a
    // negative JNI error with the slot released.
    std::int32_t register_native(JniBackend::Env env, JniBackend::Ref cls, const char* name, const char* signature,
                                 std::uint32_t guest_function, bool is_static = false);

    // Guest loader operations bound to the calling host thread. All calls use its cached carrier,
    // or the currently running guest thread for a nested Java -> load transition. A failed loader
    // lookup reads dlerror on that same guest thread.
    std::uint32_t load_library_on_current(JniBackend::Env env, const std::string& path,
                                          std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol_on_current(JniBackend::Env env, std::uint32_t handle,
                                         const std::string& name, std::string& error);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
