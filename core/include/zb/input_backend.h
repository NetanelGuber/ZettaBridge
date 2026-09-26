#pragma once

#include <cstdint>

#include "zb/jni_backend.h"

namespace zb {

// Native input pointers stay in the backend. The portable host bridge exposes only generation-
// checked guest handles and a process fd used to notify a guest looper of queue readiness.
class InputBackend {
public:
    virtual ~InputBackend() = default;
    virtual bool from_java_available() { return true; }
    virtual std::uint64_t from_java(JniBackend::Env env, JniBackend::Ref queue) = 0;
    virtual int readiness_fd(std::uint64_t queue) = 0;
    virtual int has_events(std::uint64_t queue) = 0;
    virtual int get_event(std::uint64_t queue, std::uint64_t& event) = 0;
    virtual int pre_dispatch(std::uint64_t queue, std::uint64_t event) = 0;
    virtual void finish_event(std::uint64_t queue, std::uint64_t event, int handled) = 0;
    // Returns raw scalar bits. kind is 'i' (int32/size_t), 'l' (int64), or 'f' (float).
    virtual bool query(const char* name, std::uint64_t event, std::uint32_t a,
                       std::uint32_t b, std::uint32_t c, char kind,
                       std::uint64_t& bits) = 0;
};

}  // namespace zb
