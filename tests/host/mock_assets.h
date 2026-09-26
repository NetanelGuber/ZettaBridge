#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>

#include "zb/asset_backend.h"

// In-memory AssetBackend for host tests: files are registered by name, AAssetManager_fromJava
// always succeeds with a fixed manager handle.
class MockAssetBackend final : public zb::AssetBackend {
public:
    void add_file(const std::string& name, std::string content) { files_[name] = std::move(content); }

    std::uint64_t manager_from_java(zb::JniBackend::Env env, zb::JniBackend::Ref java_manager) override {
        last_manager_env = env;
        last_manager_ref = java_manager;
        return kManagerHandle;
    }

    std::uint64_t open(std::uint64_t manager, const std::string& filename, std::int32_t mode) override {
        last_open_mode = mode;
        if (manager != kManagerHandle) return 0;
        auto it = files_.find(filename);
        if (it == files_.end()) return 0;
        const std::uint64_t handle = next_asset_++;
        open_[handle] = {it->second, 0};
        return handle;
    }

    std::int64_t length(std::uint64_t asset) override {
        auto it = open_.find(asset);
        if (it == open_.end()) return -1;
        if (length_override) return *length_override;
        return static_cast<std::int64_t>(it->second.content.size());
    }

    std::int64_t remaining_length(std::uint64_t asset) override {
        auto it = open_.find(asset);
        return it == open_.end() ? -1 : static_cast<std::int64_t>(it->second.content.size() - it->second.cursor);
    }

    std::int64_t seek(std::uint64_t asset, std::int64_t offset, int whence) override {
        auto it = open_.find(asset);
        if (it == open_.end()) return -1;
        const std::int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ?
            static_cast<std::int64_t>(it->second.cursor) : whence == SEEK_END ?
            static_cast<std::int64_t>(it->second.content.size()) : -1;
        if (base < 0 || offset < -base || offset > static_cast<std::int64_t>(it->second.content.size()) - base)
            return -1;
        it->second.cursor = static_cast<std::size_t>(base + offset);
        return base + offset;
    }

    int is_allocated(std::uint64_t asset) override { return open_.contains(asset) ? 1 : -1; }

    std::uint64_t open_dir(std::uint64_t manager, const std::string& name) override {
        if (manager != kManagerHandle) return 0;
        const std::string prefix = name.empty() ? "" : name + "/";
        Directory dir;
        for (const auto& [path, content] : files_) {
            (void)content;
            if (path.starts_with(prefix)) {
                const std::string suffix = path.substr(prefix.size());
                if (suffix.find('/') == std::string::npos) dir.names.push_back(suffix);
            }
        }
        const auto handle = next_dir_++;
        directories_[handle] = std::move(dir);
        return handle;
    }

    const char* next_file_name(std::uint64_t directory) override {
        auto it = directories_.find(directory);
        if (it == directories_.end() || it->second.cursor == it->second.names.size()) return nullptr;
        return it->second.names[it->second.cursor++].c_str();
    }

    void rewind_dir(std::uint64_t directory) override {
        auto it = directories_.find(directory);
        if (it != directories_.end()) it->second.cursor = 0;
    }

    void close_dir(std::uint64_t directory) override { directories_.erase(directory); }

    const void* buffer(std::uint64_t asset) override {
        auto it = open_.find(asset);
        if (it == open_.end() || it->second.content.empty()) return nullptr;
        return it->second.content.data();
    }

    std::int64_t read(std::uint64_t asset, void* buffer, std::size_t count) override {
        auto it = open_.find(asset);
        if (it == open_.end()) return -1;
        OpenAsset& a = it->second;
        const std::size_t remaining = a.content.size() - a.cursor;
        const std::size_t n = count < remaining ? count : remaining;
        if (n > 0) std::memcpy(buffer, a.content.data() + a.cursor, n);
        a.cursor += n;
        return static_cast<std::int64_t>(n);
    }

    void close(std::uint64_t asset) override { open_.erase(asset); }

    FileDescriptor open_file_descriptor(std::uint64_t asset) override {
        FileDescriptor fd;
        if (open_.find(asset) == open_.end()) return fd;
        fd.fd = fake_fd;
        fd.start = fake_fd_start;
        fd.length = fake_fd_length;
        return fd;
    }

    static constexpr std::uint64_t kManagerHandle = 0xA55E7;

    zb::JniBackend::Env last_manager_env = 0;
    zb::JniBackend::Ref last_manager_ref = 0;
    std::int32_t last_open_mode = -1;
    int fake_fd = 3;
    std::int64_t fake_fd_start = 1000;
    std::int64_t fake_fd_length = 42;
    std::optional<std::int64_t> length_override;

private:
    struct OpenAsset {
        std::string content;
        std::size_t cursor = 0;
    };
    struct Directory { std::vector<std::string> names; std::size_t cursor = 0; };
    std::unordered_map<std::string, std::string> files_;
    std::unordered_map<std::uint64_t, OpenAsset> open_;
    std::unordered_map<std::uint64_t, Directory> directories_;
    std::uint64_t next_asset_ = 1;
    std::uint64_t next_dir_ = 1;
};
