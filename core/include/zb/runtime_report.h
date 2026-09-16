#pragma once

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

    std::size_t unimplemented_host_calls() const;
    std::size_t proxy_loads() const;
    std::size_t jni_onload_calls() const;
    std::size_t registered_natives() const;
    // "libGLESv2.so glCreateProgram", or empty while every host call was handled.
    std::string first_unimplemented_host_call() const;

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
