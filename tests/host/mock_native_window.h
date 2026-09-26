#pragma once

#include <cstdint>
#include <array>
#include <unordered_map>

#include "zb/native_window_backend.h"

// In-memory NativeWindowBackend for host tests: fromSurface hands out fixed-geometry fake
// windows, one per distinct host surface value.
class MockNativeWindow final : public zb::NativeWindowBackend {
public:
    void* from_surface(void* env, void* surface) override {
        last_env = env;
        last_surface = surface;
        auto* window = new int(static_cast<int>(++next_window_));
        windows_[window] = 1;
        return window;
    }

    void acquire(void* window) override {
        auto it = windows_.find(window);
        if (it != windows_.end()) {
            ++it->second;
            ++acquired_;
        }
    }

    void release(void* window) override {
        auto it = windows_.find(window);
        if (it != windows_.end()) {
            ++released_;
            if (--it->second == 0) {
                windows_.erase(it);
                delete static_cast<int*>(window);
            }
        }
    }

    std::int32_t query(void* window, Query which) override {
        if (!windows_.count(window)) return -1;
        switch (which) {
        case Query::Width:
            return width;
        case Query::Height:
            return height;
        case Query::Format:
            return format;
        }
        return -1;
    }

    std::int32_t set_buffers_geometry(void* window, std::int32_t w, std::int32_t h, std::int32_t f) override {
        if (!windows_.count(window)) return -1;
        last_geometry_width = w;
        last_geometry_height = h;
        last_geometry_format = f;
        return 0;
    }

    void* to_surface(void* env, void* window) override {
        last_env = env;
        return windows_.count(window) ? last_surface : nullptr;
    }

    std::int32_t lock(void* window, Buffer& buffer, Rect* dirty) override {
        if (!windows_.count(window) || locked_) return -1;
        locked_ = true;
        buffer = {2, 2, 2, format, pixels_.data()};
        if (dirty != nullptr) *dirty = {0, 0, 2, 2};
        return 0;
    }

    std::int32_t unlock_and_post(void* window) override {
        if (!windows_.count(window) || !locked_) return -1;
        locked_ = false;
        ++posted_;
        return 0;
    }

    bool locked() const { return locked_; }
    int posted() const { return posted_; }
    const std::array<std::uint8_t, 16>& pixels() const { return pixels_; }

    int released() const { return released_; }
    int acquired() const { return acquired_; }

    void* last_env = nullptr;
    void* last_surface = nullptr;
    std::int32_t width = 1080;
    std::int32_t height = 2376;
    std::int32_t format = 1;  // PIXEL_FORMAT_RGBA_8888
    std::int32_t last_geometry_width = 0;
    std::int32_t last_geometry_height = 0;
    std::int32_t last_geometry_format = 0;

private:
    std::unordered_map<void*, unsigned> windows_;
    std::uint64_t next_window_ = 0;
    int released_ = 0;
    int acquired_ = 0;
    bool locked_ = false;
    int posted_ = 0;
    std::array<std::uint8_t, 16> pixels_{};
};
