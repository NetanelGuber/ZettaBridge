#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "zb/input_backend.h"

struct AInputQueue;
struct ALooper;

namespace zb {

class AndroidInputBackend final : public InputBackend {
public:
    bool from_java_available() override;
    std::uint64_t from_java(JniBackend::Env env, JniBackend::Ref queue) override;
    int readiness_fd(std::uint64_t queue) override;
    int has_events(std::uint64_t queue) override;
    int get_event(std::uint64_t queue, std::uint64_t& event) override;
    int pre_dispatch(std::uint64_t queue, std::uint64_t event) override;
    void finish_event(std::uint64_t queue, std::uint64_t event, int handled) override;
    bool query(const char* name, std::uint64_t event, std::uint32_t a,
               std::uint32_t b, std::uint32_t c, char kind,
               std::uint64_t& bits) override;

private:
    struct QueueState {
        AInputQueue* queue = nullptr;
        std::atomic<ALooper*> looper{nullptr};
        int fd = -1;
        std::atomic<bool> ready{false};
    };
    QueueState* state(std::uint64_t queue);
    void rearm(QueueState& state);
    static int on_ready(int fd, int events, void* data);
    static void run(QueueState* state);
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<QueueState>> queues_;
};

}  // namespace zb
