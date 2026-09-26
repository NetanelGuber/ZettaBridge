#include "zb/host_assets.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

#include "zb/asset_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/host_jni.h"
#include "zb/log.h"

namespace zb {

namespace {

constexpr std::uint64_t kMaxFilenameLength = 4096;

// Reads a NUL-terminated guest string, page by page like HostJni::Impl::read_string. ok is false
// for an unreadable address or a string longer than kMaxFilenameLength.
std::string read_guest_string(GuestMemory& memory, std::uint32_t address, bool& ok) {
    ok = false;
    if (address == 0) return {};
    std::string text;
    std::uint64_t cursor = address;
    while (cursor < kGuestSpaceSize && text.size() <= kMaxFilenameLength) {
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        const std::uint8_t* bytes = memory.host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) return {};
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            ok = true;
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
    return {};
}

}  // namespace

std::uint64_t HostAssets::require_manager(std::uint32_t handle) const {
    return managers_.get(handle).value_or(0);
}

std::uint64_t HostAssets::require_asset(std::uint32_t handle) const {
    return assets_.get(handle).value_or(0);
}

void HostAssets::free_guest(std::uint32_t address) {
    if (address == 0) return;
    if (deallocate_) {
        deallocate_(address);
        return;
    }
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::uint32_t HostAssets::allocate_guest(std::uint32_t size) {
    if (allocate_) return allocate_(size);
    GuestCall args;
    args.regs = {size, 0, 0, 0};
    const auto allocated = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    return allocated ? allocated->r0 : 0;
}

std::uint32_t HostAssets::copy_to_guest(const void* source, std::uint64_t size) {
    if (size == 0 || size > UINT32_MAX) return 0;
    const std::uint32_t address = allocate_guest(static_cast<std::uint32_t>(size));
    if (address == 0) return 0;
    std::uint8_t* destination = runtime_.memory().host_ptr(address, size, kPageWrite);
    if (destination == nullptr) {
        free_guest(address);
        return 0;
    }
    std::memcpy(destination, source, static_cast<std::size_t>(size));
    return address;
}

std::uint32_t HostAssets::asset_buffer(std::uint64_t asset) {
    const auto cached = asset_buffers_.find(asset);
    if (cached != asset_buffers_.end()) return cached->second;

    const std::int64_t length = backend_.length(asset);
    const void* host = backend_.buffer(asset);
    if (host == nullptr || length <= 0) {
        log("AAsset_getBuffer: the asset has no buffer (length %lld)", static_cast<long long>(length));
        return 0;
    }
    const auto size = static_cast<std::uint64_t>(length);
    if (size > kMaxBufferedBytes || buffered_bytes_ + size > kMaxBufferedBytes) {
        log("AAsset_getBuffer: %llu bytes exceed the buffer budget", static_cast<unsigned long long>(size));
        return 0;
    }
    const std::uint32_t address = copy_to_guest(host, size);
    if (address == 0) {
        log("AAsset_getBuffer: the guest allocator refused %llu bytes",
            static_cast<unsigned long long>(size));
        return 0;
    }
    asset_buffers_.emplace(asset, address);
    buffered_bytes_ += size;
    return address;
}

bool HostAssets::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_ASSET_HC_AAssetDir_close || index > ZB_ASSET_HC_AAsset_seek64) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& regs = thread.regs();
    switch (index) {
    case ZB_ASSET_HC_AAssetDir_close: {
        const auto directory = directories_.remove(regs[0]);
        if (directory && *directory != 0) {
            auto name = directory_names_.find(*directory);
            if (name != directory_names_.end()) {
                free_guest(name->second);
                directory_names_.erase(name);
            }
            backend_.close_dir(*directory);
        }
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAssetDir_getNextFileName: {
        const auto directory = directories_.get(regs[0]);
        regs[0] = 0;
        if (!directory || *directory == 0) return true;
        auto old = directory_names_.find(*directory);
        if (old != directory_names_.end()) {
            free_guest(old->second);
            directory_names_.erase(old);
        }
        const char* name = backend_.next_file_name(*directory);
        if (name == nullptr) return true;
        const std::size_t length = strnlen(name, kMaxFilenameLength + 1);
        if (length > kMaxFilenameLength) return true;
        const std::uint32_t address = copy_to_guest(name, length + 1);
        if (address != 0) directory_names_.emplace(*directory, address);
        regs[0] = address;
        return true;
    }
    case ZB_ASSET_HC_AAssetDir_rewind: {
        const auto directory = directories_.get(regs[0]);
        if (directory && *directory != 0) backend_.rewind_dir(*directory);
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_fromJava: {
        // regs[0] is the guest JNIEnv*: ignored, we use the real host JNIEnv of this thread
        // (0 outside an active JNI transition; the backend decides what to do with that).
        const JniBackend::Env env = host_jni_.current_env();
        const JniBackend::Ref java_manager = host_jni_.resolve_ref(regs[1], "AAssetManager_fromJava");
        const std::uint64_t manager = backend_.manager_from_java(env, java_manager);
        regs[0] = manager == 0 ? 0 : managers_.add(manager);
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_open: {
        const std::uint64_t manager = require_manager(regs[0]);
        bool ok = false;
        std::string filename;
        if (manager != 0) filename = read_guest_string(runtime_.memory(), regs[1], ok);
        std::uint64_t asset = 0;
        if (manager != 0 && ok) asset = backend_.open(manager, filename, static_cast<std::int32_t>(regs[2]));
        regs[0] = asset == 0 ? 0 : assets_.add(asset);
        if (asset != 0 && regs[0] == 0) backend_.close(asset);
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_openDir: {
        const std::uint64_t manager = require_manager(regs[0]);
        bool ok = false;
        std::string name;
        if (manager != 0) name = read_guest_string(runtime_.memory(), regs[1], ok);
        const std::uint64_t directory = manager != 0 && ok ? backend_.open_dir(manager, name) : 0;
        regs[0] = directory == 0 ? 0 : directories_.add(directory);
        if (directory != 0 && regs[0] == 0) backend_.close_dir(directory);
        return true;
    }
    case ZB_ASSET_HC_AAsset_close: {
        const std::optional<std::uint64_t> asset = assets_.remove(regs[0]);
        if (asset && *asset != 0) {
            auto buffer = asset_buffers_.find(*asset);
            if (buffer != asset_buffers_.end()) {
                free_guest(buffer->second);
                asset_buffers_.erase(buffer);
                const auto size = backend_.length(*asset);
                if (size > 0) buffered_bytes_ -= static_cast<std::uint64_t>(size);
            }
            backend_.close(*asset);
        }
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_getLength:
    case ZB_ASSET_HC_AAsset_getRemainingLength:
    case ZB_ASSET_HC_AAsset_getLength64:
    case ZB_ASSET_HC_AAsset_getRemainingLength64: {
        const std::uint64_t asset = require_asset(regs[0]);
        const bool remaining = index == ZB_ASSET_HC_AAsset_getRemainingLength ||
                               index == ZB_ASSET_HC_AAsset_getRemainingLength64;
        const bool wide = index == ZB_ASSET_HC_AAsset_getLength64 ||
                          index == ZB_ASSET_HC_AAsset_getRemainingLength64;
        std::int64_t length = asset != 0 ? (remaining ? backend_.remaining_length(asset) : backend_.length(asset)) : -1;
        if (!wide && length > std::numeric_limits<std::int32_t>::max()) {
            if (!logged_overflow_) {
                log("AAsset_getLength: asset length %lld exceeds INT32_MAX; returning -1",
                    static_cast<long long>(length));
                logged_overflow_ = true;
            }
            length = -1;
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(length));
        regs[1] = wide ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(length) >> 32) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_isAllocated: {
        const auto asset = require_asset(regs[0]);
        regs[0] = static_cast<std::uint32_t>(asset == 0 ? -1 : backend_.is_allocated(asset));
        return true;
    }
    case ZB_ASSET_HC_AAsset_read: {
        const std::uint64_t asset = require_asset(regs[0]);
        const std::uint32_t buf_addr = regs[1];
        const std::uint32_t count = regs[2];
        std::int64_t result = -1;
        if (asset != 0) {
            if (count == 0) {
                result = 0;
            } else if (count <= INT32_MAX) {
                std::uint8_t* host_buf = runtime_.memory().host_ptr(buf_addr, count, kPageWrite);
                if (host_buf != nullptr) result = backend_.read(asset, host_buf, count);
            }
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
        return true;
    }
    case ZB_ASSET_HC_AAsset_getBuffer: {
        const std::uint64_t asset = require_asset(regs[0]);
        regs[0] = asset != 0 ? asset_buffer(asset) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_openFileDescriptor:
    case ZB_ASSET_HC_AAsset_openFileDescriptor64: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int32_t fd = -1;
        if (asset != 0) {
            AssetBackend::FileDescriptor descriptor = backend_.open_file_descriptor(asset);
            const bool wide = index == ZB_ASSET_HC_AAsset_openFileDescriptor64;
            const bool fits = descriptor.fd >= 0 && descriptor.start >= 0 && descriptor.length >= 0 &&
                              (wide || (descriptor.start <= std::numeric_limits<std::int32_t>::max() &&
                                        descriptor.length <= std::numeric_limits<std::int32_t>::max()));
            if (fits) {
                const std::size_t bytes = wide ? 8 : 4;
                std::uint8_t* out_start = runtime_.memory().host_ptr(regs[1], bytes, kPageWrite);
                std::uint8_t* out_length = runtime_.memory().host_ptr(regs[2], bytes, kPageWrite);
                if (out_start != nullptr && out_length != nullptr) {
                    std::memcpy(out_start, &descriptor.start, bytes);
                    std::memcpy(out_length, &descriptor.length, bytes);
                    fd = descriptor.fd;
                } else {
                    ::close(descriptor.fd);
                }
            } else if (descriptor.fd >= 0) {
                ::close(descriptor.fd);
            }
        }
        regs[0] = static_cast<std::uint32_t>(fd);
        return true;
    }
    case ZB_ASSET_HC_AAsset_seek:
    case ZB_ASSET_HC_AAsset_seek64: {
        const auto asset = require_asset(regs[0]);
        const bool wide = index == ZB_ASSET_HC_AAsset_seek64;
        std::int64_t offset = static_cast<std::int32_t>(regs[1]);
        int whence = static_cast<std::int32_t>(regs[2]);
        if (wide) {
            offset = static_cast<std::int64_t>(static_cast<std::uint64_t>(regs[2]) |
                                               (static_cast<std::uint64_t>(regs[3]) << 32));
            const std::uint8_t* stack = runtime_.memory().host_ptr(regs[13], 4, kPageRead);
            if (stack == nullptr) {
                regs[0] = regs[1] = UINT32_MAX;
                return true;
            }
            std::memcpy(&whence, stack, 4);
        }
        std::int64_t result = asset == 0 ? -1 : backend_.seek(asset, offset, whence);
        if (!wide && result > INT32_MAX) result = -1;
        regs[0] = static_cast<std::uint32_t>(result);
        regs[1] = wide ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(result) >> 32) : 0;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
