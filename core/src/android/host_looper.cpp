#include "zb/host_looper.h"

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/runtime_report.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <string>

namespace zb {

namespace {

constexpr int kAllowNonCallbacks = 1;
constexpr int kPollWake = -1;
constexpr int kPollCallback = -2;
constexpr int kPollTimeout = -3;
constexpr int kPollError = -4;
constexpr int kEventInput = 1;
constexpr int kEventOutput = 2;
constexpr int kEventError = 4;
constexpr int kEventHangup = 8;
constexpr int kEventInvalid = 16;

int looper_events(short events) {
    int result = 0;
    if ((events & POLLIN) != 0) result |= kEventInput;
    if ((events & POLLOUT) != 0) result |= kEventOutput;
    if ((events & POLLERR) != 0) result |= kEventError;
    if ((events & POLLHUP) != 0) result |= kEventHangup;
    if ((events & POLLNVAL) != 0) result |= kEventInvalid;
    return result;
}

short poll_events(int events) {
    short result = 0;
    if ((events & kEventInput) != 0) result |= POLLIN;
    if ((events & kEventOutput) != 0) result |= POLLOUT;
    return result;
}

const char* poll_result_name(int result) {
    switch (result) {
    case kPollWake: return "wake";
    case kPollCallback: return "callback";
    case kPollTimeout: return "timeout";
    case kPollError: return "error";
    default: return result >= 0 ? "ident" : "unknown";
    }
}

void signal_eventfd(int fd) {
    const std::uint64_t one = 1;
    ssize_t written;
    do {
        written = write(fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
}

void drain_eventfd(int fd) {
    std::uint64_t value;
    ssize_t count;
    do {
        count = read(fd, &value, sizeof value);
    } while (count < 0 && errno == EINTR);
}

}  // namespace

struct HostLooper::Impl {
    struct Registration {
        int fd = -1;
        int ident = 0;
        int events = 0;
        std::uint32_t callback = 0;
        std::uint32_t data = 0;
        std::uint64_t serial = 0;
    };

    struct Looper {
        std::uint32_t handle = 0;
        int options = 0;
        std::uint32_t references = 1;
        int wake_fd = -1;
        std::unordered_map<int, Registration> registrations;
    };

    explicit Impl(LibraryRuntime& runtime_) : runtime(runtime_) {}

    ~Impl() {
        for (const auto& [handle, looper] : loopers) {
            (void)handle;
            if (looper->wake_fd >= 0) close(looper->wake_fd);
        }
    }

    // Diagnostics only (Flutter callback-looper hunt): counters and per-thread "last" state
    // pushed into the runtime report. None of this changes looper semantics.
    std::atomic<std::uint64_t> report_loopers{0};
    std::atomic<std::uint64_t> report_fds_added{0};
    std::atomic<std::uint64_t> report_fds_removed{0};
    std::atomic<std::uint64_t> report_polls{0};
    std::atomic<std::uint64_t> report_polls_wake{0};
    std::atomic<std::uint64_t> report_polls_timeout{0};
    std::atomic<std::uint64_t> report_polls_callback{0};
    std::atomic<std::uint64_t> report_polls_error{0};
    std::atomic<std::uint64_t> report_polls_ident{0};
    std::atomic<std::uint64_t> report_callbacks{0};
    std::atomic<std::uint64_t> report_callbacks_unregistered{0};
    std::atomic<std::uint64_t> report_wakes{0};

    static constexpr std::size_t kMaxReportSlots = 4;
    std::mutex report_mutex;
    std::vector<std::int32_t> report_slot_tids;

    void report_loopers_line() {
        runtime_report().note_looper_detail(
            "loopers", std::to_string(report_loopers.load(std::memory_order_relaxed)), true);
    }

    void report_fds_line() {
        char buf[96];
        std::snprintf(buf, sizeof buf, "added=%llu removed=%llu",
                      static_cast<unsigned long long>(report_fds_added.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_fds_removed.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("fds", buf, true);
    }

    void report_polls_line() {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "total=%llu wake=%llu timeout=%llu callback=%llu error=%llu ident=%llu",
                      static_cast<unsigned long long>(report_polls.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_wake.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_timeout.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_callback.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_error.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_ident.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("polls", buf, true);
    }

    void report_callbacks_line() {
        char buf[128];
        std::snprintf(buf, sizeof buf, "dispatched=%llu unregistered=%llu",
                      static_cast<unsigned long long>(report_callbacks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_callbacks_unregistered.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("callbacks", buf, true);
    }

    void report_wakes_line() {
        runtime_report().note_looper_detail(
            "wakes", std::to_string(report_wakes.load(std::memory_order_relaxed)), true);
    }

    void report_last(const GuestThread& thread, std::size_t fd_count, int result) {
        std::size_t index;
        {
            std::lock_guard<std::mutex> lock(report_mutex);
            const auto it = std::find(report_slot_tids.begin(), report_slot_tids.end(), thread.tid);
            if (it != report_slot_tids.end()) {
                index = static_cast<std::size_t>(it - report_slot_tids.begin());
            } else if (report_slot_tids.size() < kMaxReportSlots) {
                index = report_slot_tids.size();
                report_slot_tids.push_back(thread.tid);
            } else {
                return;
            }
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "tid=%d fds=%zu result=%s", static_cast<int>(thread.tid),
                      fd_count, poll_result_name(result));
        runtime_report().note_looper_detail(
            "last-" + std::to_string(index), buf, true);
    }

    std::uint32_t argument(GuestThread& thread, unsigned position, bool& valid) const {
        if (position < 4) return thread.regs()[position];
        const std::uint64_t address = static_cast<std::uint64_t>(thread.regs()[13]) +
                                      4u * static_cast<std::uint64_t>(position - 4);
        if (address > UINT32_MAX) {
            valid = false;
            return 0;
        }
        const std::uint8_t* source = runtime.memory().host_ptr(
            static_cast<std::uint32_t>(address), sizeof(std::uint32_t), kPageRead);
        if (source == nullptr) {
            valid = false;
            return 0;
        }
        std::uint32_t value;
        std::memcpy(&value, source, sizeof value);
        return value;
    }

    bool write_guest(std::uint32_t address, std::uint32_t value) const {
        if (address == 0) return true;
        std::uint8_t* target = runtime.memory().host_ptr(address, sizeof value, kPageWrite);
        if (target == nullptr) return false;
        std::memcpy(target, &value, sizeof value);
        return true;
    }

    std::uint32_t prepare(GuestThread& thread, int options) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto existing = thread_loopers.find(&thread);
        if (existing != thread_loopers.end()) return existing->second;
        if ((options & ~kAllowNonCallbacks) != 0) return 0;

        const int wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_fd < 0) return 0;
        auto looper = std::make_unique<Looper>();
        looper->handle = next_handle;
        looper->options = options;
        looper->wake_fd = wake_fd;
        const std::uint32_t handle = looper->handle;
        next_handle += 4;
        thread_loopers.emplace(&thread, handle);
        loopers.emplace(handle, std::move(looper));
        report_loopers.fetch_add(1, std::memory_order_relaxed);
        report_loopers_line();
        return handle;
    }

    std::uint32_t for_thread(GuestThread& thread) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = thread_loopers.find(&thread);
        return it == thread_loopers.end() ? 0 : it->second;
    }

    void acquire(std::uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = loopers.find(handle);
        if (it != loopers.end() && it->second->references !=
                                       std::numeric_limits<std::uint32_t>::max()) {
            ++it->second->references;
        }
    }

    void release(std::uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = loopers.find(handle);
        if (it != loopers.end() && it->second->references > 1) --it->second->references;
    }

    int add_fd(std::uint32_t handle, int fd, int ident, int events,
               std::uint32_t callback, std::uint32_t data) {
        int wake_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end() || fd < 0 || events == 0 ||
                (events & ~(kEventInput | kEventOutput)) != 0) {
                return -1;
            }
            Looper& looper = *it->second;
            if (callback == 0 &&
                (ident < 0 || (looper.options & kAllowNonCallbacks) == 0)) {
                return -1;
            }
            looper.registrations[fd] =
                Registration{fd, ident, events, callback, data, next_serial++};
            wake_fd = looper.wake_fd;
        }
        signal_eventfd(wake_fd);
        report_fds_added.fetch_add(1, std::memory_order_relaxed);
        report_fds_line();
        return 1;
    }

    int remove_fd(std::uint32_t handle, int fd) {
        int wake_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end()) return -1;
            Looper& looper = *it->second;
            if (looper.registrations.erase(fd) == 0) return 0;
            wake_fd = looper.wake_fd;
        }
        signal_eventfd(wake_fd);
        report_fds_removed.fetch_add(1, std::memory_order_relaxed);
        report_fds_line();
        return 1;
    }

    void wake(std::uint32_t handle) {
        int wake_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it != loopers.end()) wake_fd = it->second->wake_fd;
        }
        if (wake_fd >= 0) signal_eventfd(wake_fd);
        report_wakes.fetch_add(1, std::memory_order_relaxed);
        report_wakes_line();
    }

    int poll_once(GuestThread& thread, int timeout, std::uint32_t out_fd,
                  std::uint32_t out_events, std::uint32_t out_data) {
        std::uint32_t looper_handle = 0;
        int wake_fd = -1;
        std::vector<Registration> registrations;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto thread_it = thread_loopers.find(&thread);
            if (thread_it == thread_loopers.end()) return kPollError;
            const auto looper_it = loopers.find(thread_it->second);
            if (looper_it == loopers.end()) return kPollError;
            looper_handle = thread_it->second;
            wake_fd = looper_it->second->wake_fd;
            registrations.reserve(looper_it->second->registrations.size());
            for (const auto& [fd, registration] : looper_it->second->registrations) {
                (void)fd;
                registrations.push_back(registration);
            }
        }

        // Diagnostics only: every exit path below runs through `finish`, which counts the
        // poll and records this thread's "last" state in the runtime report.
        auto finish = [&](int result) {
            report_polls.fetch_add(1, std::memory_order_relaxed);
            switch (result) {
            case kPollWake: report_polls_wake.fetch_add(1, std::memory_order_relaxed); break;
            case kPollCallback: report_polls_callback.fetch_add(1, std::memory_order_relaxed); break;
            case kPollTimeout: report_polls_timeout.fetch_add(1, std::memory_order_relaxed); break;
            case kPollError: report_polls_error.fetch_add(1, std::memory_order_relaxed); break;
            default: if (result >= 0) report_polls_ident.fetch_add(1, std::memory_order_relaxed); break;
            }
            report_polls_line();
            report_last(thread, registrations.size(), result);
            return result;
        };

        std::vector<pollfd> poll_fds;
        poll_fds.reserve(registrations.size() + 1);
        poll_fds.push_back(pollfd{wake_fd, POLLIN, 0});
        for (const Registration& registration : registrations) {
            poll_fds.push_back(pollfd{registration.fd, poll_events(registration.events), 0});
        }

        int ready;
        do {
            ready = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return finish(kPollError);
        if (ready == 0) return finish(kPollTimeout);

        const bool woke = poll_fds[0].revents != 0;
        if (woke) drain_eventfd(wake_fd);
        bool callback_invoked = false;
        const Registration* non_callback = nullptr;
        int non_callback_events = 0;
        for (std::size_t i = 0; i < registrations.size(); ++i) {
            const int events = looper_events(poll_fds[i + 1].revents);
            if (events == 0) continue;
            const Registration& registration = registrations[i];
            if (registration.callback != 0) {
                GuestCall call;
                call.regs = {static_cast<std::uint32_t>(registration.fd),
                             static_cast<std::uint32_t>(events), registration.data, 0};
                const auto result = runtime.call_on_current(registration.callback, call);
                if (!result) return finish(kPollError);
                callback_invoked = true;
                report_callbacks.fetch_add(1, std::memory_order_relaxed);
                if (result->r0 == 0) {
                    report_callbacks_unregistered.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto looper_it = loopers.find(looper_handle);
                    if (looper_it != loopers.end()) {
                        auto current = looper_it->second->registrations.find(registration.fd);
                        if (current != looper_it->second->registrations.end() &&
                            current->second.serial == registration.serial) {
                            looper_it->second->registrations.erase(current);
                        }
                    }
                }
                report_callbacks_line();
                continue;
            }
            if (non_callback == nullptr) {
                non_callback = &registration;
                non_callback_events = events;
            }
        }
        if (callback_invoked) return finish(kPollCallback);
        if (non_callback != nullptr) {
            if (!write_guest(out_fd, static_cast<std::uint32_t>(non_callback->fd)) ||
                !write_guest(out_events, static_cast<std::uint32_t>(non_callback_events)) ||
                !write_guest(out_data, non_callback->data)) {
                return finish(kPollError);
            }
            return finish(non_callback->ident);
        }
        return finish(woke ? kPollWake : kPollError);
    }

    LibraryRuntime& runtime;
    std::mutex mutex;
    std::unordered_map<const GuestThread*, std::uint32_t> thread_loopers;
    std::unordered_map<std::uint32_t, std::unique_ptr<Looper>> loopers;
    std::uint32_t next_handle = 0x7a000000u;
    std::uint64_t next_serial = 1;
};

HostLooper::HostLooper(LibraryRuntime& runtime) : impl_(std::make_unique<Impl>(runtime)) {}
HostLooper::~HostLooper() = default;

bool HostLooper::handle_host_call(std::uint32_t index, GuestThread& thread) {
    bool valid = true;
    const std::uint32_t r0 = impl_->argument(thread, 0, valid);
    const std::uint32_t r1 = impl_->argument(thread, 1, valid);
    const std::uint32_t r2 = impl_->argument(thread, 2, valid);
    const std::uint32_t r3 = impl_->argument(thread, 3, valid);
    auto finish = [&](std::int32_t result) {
        thread.regs()[0] = static_cast<std::uint32_t>(result);
        thread.regs()[1] = 0;
        return true;
    };

    switch (index) {
    case ZB_COMPAT_HC_ALooper_forThread:
        return finish(static_cast<std::int32_t>(impl_->for_thread(thread)));
    case ZB_COMPAT_HC_ALooper_prepare:
        return finish(static_cast<std::int32_t>(impl_->prepare(thread, static_cast<int>(r0))));
    case ZB_COMPAT_HC_ALooper_acquire:
        impl_->acquire(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_release:
        impl_->release(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_addFd: {
        const std::uint32_t callback = impl_->argument(thread, 4, valid);
        const std::uint32_t data = impl_->argument(thread, 5, valid);
        if (!valid) return finish(-1);
        return finish(impl_->add_fd(r0, static_cast<std::int32_t>(r1),
                                    static_cast<std::int32_t>(r2), static_cast<int>(r3),
                                    callback, data));
    }
    case ZB_COMPAT_HC_ALooper_removeFd:
        return finish(impl_->remove_fd(r0, static_cast<std::int32_t>(r1)));
    case ZB_COMPAT_HC_ALooper_wake:
        impl_->wake(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_pollOnce:
        return finish(impl_->poll_once(thread, static_cast<std::int32_t>(r0), r1, r2, r3));
    default:
        return false;
    }
}

}  // namespace zb
