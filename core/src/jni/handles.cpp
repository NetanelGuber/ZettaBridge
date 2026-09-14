#include "zb/jni_handles.h"

namespace zb {

std::optional<HandleKind> handle_kind(std::uint32_t handle) {
    const std::uint32_t bits = handle & 3u;
    if (bits == 0) return std::nullopt;
    return static_cast<HandleKind>(bits);
}

void LocalHandles::push_frame() {
    frame_starts_.push_back(refs_.size());
}

std::vector<std::uint64_t> LocalHandles::pop_frame() {
    std::vector<std::uint64_t> released;
    if (frame_starts_.empty()) return released;
    const std::size_t start = frame_starts_.back();
    frame_starts_.pop_back();
    for (std::size_t i = start; i < refs_.size(); ++i) {
        if (refs_[i] != 0) released.push_back(refs_[i]);
    }
    refs_.resize(start);
    return released;
}

std::uint32_t LocalHandles::add(std::uint64_t host_ref) {
    if (host_ref == 0) return 0;
    if (frame_starts_.empty()) push_frame();
    refs_.push_back(host_ref);
    return make_handle(HandleKind::Local, static_cast<std::uint32_t>(refs_.size() - 1));
}

std::optional<std::uint64_t> LocalHandles::get(std::uint32_t handle) const {
    if (handle == 0) return 0;
    if (handle_kind(handle) != HandleKind::Local) return std::nullopt;
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    return refs_[index];
}

std::optional<std::uint64_t> LocalHandles::remove(std::uint32_t handle) {
    if (handle == 0) return 0;
    const std::optional<std::uint64_t> ref = get(handle);
    if (!ref) return std::nullopt;
    refs_[handle_index(handle)] = 0;
    return ref;
}

std::uint32_t GlobalHandles::add(std::uint64_t host_ref) {
    if (host_ref == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
        refs_[index] = host_ref;
    } else {
        refs_.push_back(host_ref);
        index = static_cast<std::uint32_t>(refs_.size() - 1);
    }
    return make_handle(kind_, index);
}

std::optional<std::uint64_t> GlobalHandles::get(std::uint32_t handle) const {
    if (handle == 0) return 0;
    if (handle_kind(handle) != kind_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    return refs_[index];
}

std::optional<std::uint64_t> GlobalHandles::remove(std::uint32_t handle) {
    if (handle == 0) return 0;
    if (handle_kind(handle) != kind_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    const std::uint64_t ref = refs_[index];
    refs_[index] = 0;
    free_.push_back(index);
    return ref;
}

std::uint32_t IdTable::intern(std::uint64_t host_id) {
    if (host_id == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(host_id);
    if (it != index_.end()) return it->second;
    ids_.push_back(host_id);
    const auto id = static_cast<std::uint32_t>(ids_.size());
    index_.emplace(host_id, id);
    return id;
}

std::optional<std::uint64_t> IdTable::get(std::uint32_t id) const {
    if (id == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (id > ids_.size()) return std::nullopt;
    return ids_[id - 1];
}

}  // namespace zb
