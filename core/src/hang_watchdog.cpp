// Hang watchdog: per-thread last-activity slots (lock-free, allocation-free) plus a background
// sampler that turns "same activity, same counter, two samples running" into a report note. No
// guest semantics change here; a stuck thread is only ever described, never touched.
#include "zb/hang_watchdog.h"

#include <atomic>
#include <cstdio>
#include <thread>

#include "zb/host_jni.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// Fixed-size, never-grows table of per-thread activity slots. A guest thread claims a slot the
// first time it is seen and keeps it (tids are not reused across a run in a way that matters for
// diagnostics); a slot is identified by `tid` with 0 meaning free.
constexpr std::size_t kMaxSlots = 256;

struct Slot {
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(ThreadActivityKind::kNone)};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint64_t> counter{0};
};

Slot g_slots[kMaxSlots];

}  // namespace

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id) {
    if (tid == 0) return;
    // Find an existing slot for this tid, or claim the first free one. A relaxed CAS race
    // between two threads claiming the same free slot for different tids is possible but
    // vanishingly rare and self-heals: the loser just tries the next slot next call.
    Slot* free_slot = nullptr;
    for (Slot& slot : g_slots) {
        const std::int32_t current = slot.tid.load(std::memory_order_relaxed);
        if (current == tid) {
            slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
            slot.id.store(id, std::memory_order_relaxed);
            slot.counter.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) return;  // table full; diagnostics only, never blocks the guest
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
        // Lost the race; the winner's slot is not this tid's, try once more on the next call.
        return;
    }
    free_slot->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
    free_slot->id.store(id, std::memory_order_relaxed);
    free_slot->counter.store(1, std::memory_order_relaxed);
}

std::vector<ThreadActivitySample> snapshot_thread_activity() {
    std::vector<ThreadActivitySample> out;
    for (Slot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid == 0) continue;
        ThreadActivitySample sample;
        sample.tid = tid;
        sample.kind = static_cast<ThreadActivityKind>(slot.kind.load(std::memory_order_relaxed));
        sample.id = slot.id.load(std::memory_order_relaxed);
        sample.counter = slot.counter.load(std::memory_order_relaxed);
        out.push_back(sample);
    }
    return out;
}

std::string describe_thread_activity(const ThreadActivitySample& sample) {
    char text[64];
    switch (sample.kind) {
    case ThreadActivityKind::kSyscall:
        std::snprintf(text, sizeof text, "sys:%s", syscall_name(sample.id));
        return text;
    case ThreadActivityKind::kHostCall:
        std::snprintf(text, sizeof text, "host:0x%x", sample.id);
        return text;
    case ThreadActivityKind::kNone:
    default:
        return "(none)";
    }
}

HangWatchdog::HangWatchdog(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

bool HangWatchdog::sample(Clock::time_point now) {
    const std::vector<ThreadActivitySample> current = snapshot_();

    std::vector<State> next;
    next.reserve(current.size());
    for (const ThreadActivitySample& sample : current) {
        State* prior = nullptr;
        for (State& state : previous_) {
            if (state.tid == sample.tid) {
                prior = &state;
                break;
            }
        }
        State state;
        state.tid = sample.tid;
        state.kind = sample.kind;
        state.id = sample.id;
        state.counter = sample.counter;
        if (prior != nullptr && prior->kind == sample.kind && prior->id == sample.id &&
            prior->counter == sample.counter) {
            state.since = prior->since;
            state.unchanged_samples = prior->unchanged_samples + 1;
        } else {
            state.since = now;
            state.unchanged_samples = 0;
        }
        next.push_back(state);
    }
    previous_ = std::move(next);

    if (notes_written_ >= kMaxNotes) return false;

    std::vector<const State*> stuck;
    for (const State& state : previous_) {
        if (state.unchanged_samples >= kStuckThreshold) stuck.push_back(&state);
    }
    if (stuck.empty()) return false;

    std::string value;
    std::size_t shown = 0;
    for (const State* state : stuck) {
        if (shown >= kMaxThreadsShown) break;
        if (shown > 0) value += " | ";
        ThreadActivitySample sample;
        sample.tid = state->tid;
        sample.kind = state->kind;
        sample.id = state->id;
        sample.counter = state->counter;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->since).count();
        value += std::to_string(state->tid) + "=" + describe_thread_activity(sample) +
                 " x" + std::to_string(state->counter) + " stuck=" + std::to_string(seconds);
        ++shown;
    }
    value += " | recent-jni: " + jni_recent_calls();

    ++notes_written_;
    runtime_report().note_watch_detail(std::to_string(notes_written_), value);
    return true;
}

void HangWatchdog::run_forever() {
    for (;;) {
        std::this_thread::sleep_for(kInterval);
        sample(Clock::now());
    }
}

void start_hang_watchdog() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::thread([]() {
        HangWatchdog watchdog;
        watchdog.run_forever();
    }).detach();
}

}  // namespace zb
