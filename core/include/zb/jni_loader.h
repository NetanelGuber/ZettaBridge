#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include "zb/jni_backend.h"

namespace zb {

class HostJni;

struct JniLoadReport {
    bool ok = false;
    std::uint32_t guest_handle = 0;
    std::int32_t jni_version = 0x00010006;
    std::size_t bound_methods = 0;
    std::size_t skipped_classes = 0;
    std::string error;
};

// Loads one arm32 JNI library, binds its statically named Java_* exports through HostJni
// thunks, then invokes its guest JNI_OnLoad on the same calling-thread carrier.
class JniLoader {
public:
    JniLoader(HostJni& host_jni, JniBackend& backend) : host_jni_(host_jni), backend_(backend) {}

    JniLoadReport load(JniBackend::Env env, const std::string& path, std::uint32_t guest_flags);

private:
    void log_missing_class_once(const std::string& name);

    HostJni& host_jni_;
    JniBackend& backend_;
    std::mutex missing_mutex_;
    std::unordered_set<std::string> missing_classes_;
};

}  // namespace zb
