#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zb {

// Diagnostics that must survive a silent :guest death. OxygenOS drops third-party logcat
// output, so everything a device run has to prove is collected here and rendered as short
// plain text by text(); the Android side persists that text to a file on every change.
//
// Every note_* is safe from any thread, changes no guest semantics, and is bounded: the
// distinct host-call list stops at kMaxDistinctHostCalls entries and each per-library list at
// kMaxLibraries.
class RuntimeReport {
public:
    static constexpr std::size_t kMaxDistinctHostCalls = 16;
    static constexpr std::size_t kMaxLibraries = 32;
    // Recorded error and exit texts are folded to one line and cut to this length.
    static constexpr std::size_t kMaxDetail = 240;

    // Called after every change, with the report unlocked. `structural` is true when the report
    // gained a line (a new distinct host call, a load, a JNI_OnLoad, the exit reason) and false
    // when only a counter moved. A persisting observer writes structural changes at once and may
    // throttle the rest, so a hot loop of unimplemented GLES calls cannot stall the guest.
    using Observer = std::function<void(bool structural)>;

    void set_observer(Observer observer);

    void note_plugin(const std::string& plugin_root, std::uint32_t target_sdk);
    // One host call that had no handler. The caller still writes r0 = 0 and continues.
    void note_unimplemented_host_call(std::uint32_t index, const char* library, const char* function);
    void note_proxy_loaded(const std::string& library, std::int32_t jni_version);
    void note_proxy_failed(const std::string& library, const std::string& error);
    void note_jni_onload(const std::string& library, bool ok, std::int32_t jni_version);
    void note_registered_native();
    // The first reason wins: a crash report says more than the exit status that follows it.
    void note_guest_exit(const std::string& reason);

    // GLES section (Phase 5 Task 8): proves or disproves that guest GL calls arrive on a host
    // thread with an EGL context current.
    // One GL host call, always counted; the first call also records its function name and the
    // calling host thread id (gettid()).
    void note_gl_call(const char* function, std::uint64_t host_tid);
    // Whether eglGetCurrentContext() != EGL_NO_CONTEXT on the thread of the first GL call.
    // Recorded once; later calls are ignored.
    void note_gl_egl_context(bool current);
    // The first glGetError() result that is not GL_NO_ERROR. Recorded once.
    void note_gl_error(const char* function, std::uint32_t error);
    // Free-form GL visibility diagnostics (black-screen hunt): one "gl-<key>: <value>" line per
    // key, in first-note order. A known key keeps its value unless `overwrite` is set. At most
    // kMaxGlDetails keys; later new keys are dropped.
    void note_gl_detail(const std::string& key, const std::string& value, bool overwrite);

    // Crash diagnostics (precise-fault hunt): one "crash-<key>: <value>" line per key, in
    // first-note order, later notes of the same key overwriting the value. At most
    // kMaxCrashDetails keys; later new keys are dropped.
    // JNI diagnostics, printed as "jni-<key>: <value>" lines.
    void note_jni_detail(const std::string& key, const std::string& value, bool overwrite);
    void note_crash_detail(const std::string& key, const std::string& value);

    // Hang watchdog (no crash, no progress): one "watch-<key>: <value>" line per key, in
    // first-note order, later notes of the same key overwriting the value. At most
    // kMaxWatchDetails keys; later new keys are dropped.
    void note_watch_detail(const std::string& key, const std::string& value);

    // EGL section (Phase 7a Task 7): proves or disproves that the guest built an EGL context and
    // surface and is presenting frames on the thread GL calls arrive on.
    // A created EGL object (context or window surface): one "egl-<key>: <value>" line per key,
    // in first-note order, later notes of the same key overwriting the value. At most
    // kMaxGlDetails keys; later new keys are dropped.
    void note_egl_object(const std::string& key, const std::string& value);
    // The host tid of the most recent eglMakeCurrent. Bumps a generation counter HostGl polls
    // (egl_current_generation()) to know when to sample the thread of the next gl* call.
    void note_egl_current(std::uint64_t host_tid);
    // The host tid of the first gl* call sampled after an eglMakeCurrent. Compared against the
    // eglMakeCurrent thread in text(); a mismatch means GL calls are not landing on the thread
    // that made the context current.
    void note_gl_thread(std::uint64_t host_tid);
    // One eglSwapBuffers call, always counted.
    void note_egl_swap();
    // The first EGL_BAD_* result from an EGL call. Recorded once.
    void note_egl_error(const char* function, std::uint32_t error);
    // Bumped by note_egl_current; lets HostGl notice a new eglMakeCurrent with one relaxed atomic
    // load per gl* call instead of any locking.
    std::uint64_t egl_current_generation() const;

    std::size_t unimplemented_host_calls() const;
    std::size_t proxy_loads() const;
    std::size_t jni_onload_calls() const;
    std::size_t registered_natives() const;
    // "libGLESv2.so glCreateProgram", or empty while every host call was handled.
    std::string first_unimplemented_host_call() const;
    std::uint64_t gl_calls() const;

    // The whole report: one "key: value" line per fact, in a fixed order.
    std::string text() const;

    // Tests only: drops every recorded value and the observer.
    void clear();

private:
    struct HostCall {
        std::uint32_t index = 0;
        const char* library = "?";
        const char* function = "?";
        std::uint64_t count = 0;
    };
    struct Load {
        std::string library;
        bool ok = false;
        std::int32_t jni_version = 0;
        std::string error;
    };

    // Copies the observer under the lock; the caller runs it after unlocking.
    std::shared_ptr<Observer> take_observer() const;

    mutable std::mutex mutex_;
    std::shared_ptr<Observer> observer_;
    std::string plugin_root_;
    std::uint32_t target_sdk_ = 0;
    std::vector<HostCall> host_calls_;
    std::size_t distinct_host_calls_ = 0;
    std::uint64_t host_call_total_ = 0;
    std::vector<Load> proxy_loads_;
    std::size_t proxy_load_total_ = 0;
    std::size_t proxy_failure_total_ = 0;
    std::vector<Load> onloads_;
    std::size_t onload_total_ = 0;
    std::uint64_t registered_natives_ = 0;
    std::string exit_reason_;

    std::uint64_t gl_call_total_ = 0;
    std::string gl_first_call_function_;
    std::uint64_t gl_first_call_tid_ = 0;
    bool gl_egl_context_known_ = false;
    bool gl_egl_context_current_ = false;
    bool gl_error_known_ = false;
    std::string gl_error_function_;
    std::uint32_t gl_error_value_ = 0;
    static constexpr std::size_t kMaxGlDetails = 96;
    std::vector<std::pair<std::string, std::string>> gl_details_;

    static constexpr std::size_t kMaxCrashDetails = 32;
    std::vector<std::pair<std::string, std::string>> crash_details_;
    static constexpr std::size_t kMaxJniDetails = 16;
    std::vector<std::pair<std::string, std::string>> jni_details_;
    static constexpr std::size_t kMaxWatchDetails = 8;
    std::vector<std::pair<std::string, std::string>> watch_details_;

    std::vector<std::pair<std::string, std::string>> egl_objects_;
    bool egl_current_known_ = false;
    std::uint64_t egl_current_tid_ = 0;
    std::atomic<std::uint64_t> egl_current_generation_{0};
    bool gl_thread_known_ = false;
    std::uint64_t gl_thread_tid_ = 0;
    std::uint64_t egl_swap_total_ = 0;
    bool egl_error_known_ = false;
    std::string egl_error_function_;
    std::uint32_t egl_error_value_ = 0;
};

// The one report of this process.
RuntimeReport& runtime_report();

// Installs an observer on `report` that rewrites `path` with text() whenever the report
// changes, atomically (a sibling temporary file, fsync, rename), so a process that dies
// silently still leaves the last state on disk. Structural changes are written at once,
// counter-only changes at most once per min_interval. Returns false, and installs nothing, when
// the first write fails.
bool write_runtime_report_to(RuntimeReport& report, const std::string& path,
                             std::chrono::milliseconds min_interval = std::chrono::milliseconds(1000));

}  // namespace zb
