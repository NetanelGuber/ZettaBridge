#pragma once

#include "zb/asset_backend.h"

#include <mutex>
#include <unordered_map>

namespace zb {

// The real AssetBackend (Phase 5 Task 7): a thin wrapper over the NDK
// <android/asset_manager_jni.h> / <android/asset_manager.h>. Opaque host handles are the
// AAssetManager*/AAsset* pointers themselves, reinterpret_cast to/from std::uint64_t; HostAssets
// never lets them reach the guest, only 32-bit handles into its own tables.
class AndroidAssetBackend final : public AssetBackend {
public:
    std::uint64_t manager_from_java(JniBackend::Env env, JniBackend::Ref java_manager) override;
    std::uint64_t open(std::uint64_t manager, const std::string& filename, std::int32_t mode) override;
    std::int64_t length(std::uint64_t asset) override;
    std::int64_t remaining_length(std::uint64_t asset) override;
    std::int64_t seek(std::uint64_t asset, std::int64_t offset, int whence) override;
    int is_allocated(std::uint64_t asset) override;
    std::uint64_t open_dir(std::uint64_t manager, const std::string& name) override;
    const char* next_file_name(std::uint64_t directory) override;
    void rewind_dir(std::uint64_t directory) override;
    void close_dir(std::uint64_t directory) override;
    const void* buffer(std::uint64_t asset) override;
    std::int64_t read(std::uint64_t asset, void* buffer, std::size_t count) override;
    void close(std::uint64_t asset) override;
    FileDescriptor open_file_descriptor(std::uint64_t asset) override;

private:
    // AAssetManager_fromJava's native pointer is only valid while its Java owner lives.
    // Managers have no NDK close API, so retain one global reference per distinct manager for
    // this process-lifetime backend. The guest sees only generation-checked 32-bit handles.
    std::mutex managers_mutex_;
    std::unordered_map<std::uint64_t, std::uint64_t> manager_owners_;
};

}  // namespace zb
