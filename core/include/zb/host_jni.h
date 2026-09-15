#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "zb/guest_thread.h"
#include "zb/jni_backend.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace zb {

// The host side of the guest JNIEnv (guest/zbjni/zbjni.c -> libzbjni.so). It serves the flat JNI
// host calls against a JniBackend, owns the 32-bit handle and id tables, and runs guest native
// code on behalf of Java callers.
//
// One instance per process. Like the LibraryRuntime it is process-lifetime and must outlive
// every guest thread.
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

    HostJni(LibraryRuntime& runtime, JniBackend& backend);
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

    // Runs a guest function as native code called from Java on the calling host thread, whose
    // host JNIEnv is env. Uses the guest thread this host thread already runs, or else this host
    // thread's cached carrier. Opens a local frame for the call; return_type 'L' converts the
    // returned handle before the frame closes. nullopt if the frame could not be opened (a Java
    // exception is pending) or the guest call failed.
    std::optional<NativeResult> call_native(JniBackend::Env env, char return_type, std::uint32_t function,
                                            const BuildCall& build);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
