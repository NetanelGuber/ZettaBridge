#include "zb/host_bitmap.h"

#include <algorithm>
#include <cstring>

#include "zb/guest_memory.h"
#include "zb/host_jni.h"
#include "zb/platform_compat_hostcalls.h"

namespace zb {

void HostBitmap::free_guest(std::uint32_t address) {
    if (address == 0) return;
    if (deallocate_) {
        deallocate_(address);
        return;
    }
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::uint32_t HostBitmap::allocate_guest(std::uint32_t bytes) {
    if (allocate_) return allocate_(bytes);
    GuestCall args;
    args.regs = {bytes, 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    return result ? result->r0 : 0;
}

bool HostBitmap::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_COMPAT_HC_AndroidBitmap_getInfo ||
        index > ZB_COMPAT_HC_AndroidBitmap_unlockPixels) return false;
    auto& regs = thread.regs();
    const auto context = context_ ? context_(regs[1]) :
        std::pair{jni_.current_env(), jni_.resolve_ref(regs[1], "AndroidBitmap")};
    const auto [env, bitmap] = context;
    auto finish = [&](int result) {
        regs[0] = static_cast<std::uint32_t>(result);
        regs[1] = 0;
        return true;
    };
    if (env == 0 || bitmap == 0) return finish(-1);  // BAD_PARAMETER

    if (index == ZB_COMPAT_HC_AndroidBitmap_getInfo) {
        auto* target = runtime_.memory().host_ptr(regs[2], sizeof(BitmapInfo), kPageWrite);
        if (target == nullptr) return finish(-1);
        BitmapInfo info;
        const int result = backend_.get_info(env, bitmap, info);
        if (result == 0) std::memcpy(target, &info, sizeof info);
        return finish(result);
    }

    if (index == ZB_COMPAT_HC_AndroidBitmap_lockPixels) {
        auto* target = runtime_.memory().host_ptr(regs[2], 4, kPageWrite);
        if (target == nullptr) return finish(-1);
        BitmapInfo info;
        int result = backend_.get_info(env, bitmap, info);
        if (result != 0) return finish(result);
        const std::uint32_t pixel_bytes = info.format == 1 || info.format == 10 ? 4 :
                                          info.format == 4 || info.format == 7 ? 2 :
                                          info.format == 8 ? 1 : info.format == 9 ? 8 : 0;
        if (info.width == 0 || info.height == 0 || pixel_bytes == 0 ||
            static_cast<std::uint64_t>(info.width) * pixel_bytes > info.stride ||
            info.stride > (64ull << 20) ||
            info.height > (64ull << 20) / info.stride ||
            (info.flags & 0x80000000u) != 0) return finish(-1);
        const std::uint64_t bytes = static_cast<std::uint64_t>(info.height) * info.stride;
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        for (const auto& lock : locks_) {
            if (backend_.same_object(env, bitmap, lock.bitmap)) return finish(-1);
        }
        void* pixels = nullptr;
        result = backend_.lock_pixels(env, bitmap, pixels);
        if (result != 0) return finish(result);
        if (pixels == nullptr) {
            (void)backend_.unlock_pixels(env, bitmap);
            return finish(-1);
        }
        const std::uint32_t address = allocate_guest(static_cast<std::uint32_t>(bytes));
        if (address == 0) {
            (void)backend_.unlock_pixels(env, bitmap);
            return finish(-3);  // ALLOCATION_FAILED
        }
        if (static_cast<std::uint64_t>(regs[2]) < static_cast<std::uint64_t>(address) + bytes &&
            static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(regs[2]) + 4) {
            free_guest(address);
            (void)backend_.unlock_pixels(env, bitmap);
            return finish(-1);
        }
        auto* guest = runtime_.memory().host_ptr(address, bytes, kPageRead | kPageWrite);
        if (guest == nullptr) {
            free_guest(address);
            (void)backend_.unlock_pixels(env, bitmap);
            return finish(-1);
        }
        const auto retained = backend_.retain(env, bitmap);
        if (retained == 0) {
            free_guest(address);
            (void)backend_.unlock_pixels(env, bitmap);
            return finish(-3);
        }
        std::memcpy(guest, pixels, static_cast<std::size_t>(bytes));
        std::memcpy(target, &address, 4);
        locks_.push_back({retained, address, bytes, pixels, std::this_thread::get_id()});
        return finish(0);
    }

    Lock lock{};
    {
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        auto it = std::find_if(locks_.begin(), locks_.end(), [&](const Lock& current) {
            return backend_.same_object(env, bitmap, current.bitmap);
        });
        if (it == locks_.end() || it->owner != std::this_thread::get_id()) return finish(-1);
        lock = *it;
        locks_.erase(it);
    }
    const auto* guest = runtime_.memory().host_ptr(lock.address, lock.bytes, kPageRead);
    if (guest != nullptr) std::memcpy(lock.pixels, guest, static_cast<std::size_t>(lock.bytes));
    free_guest(lock.address);
    const int result = backend_.unlock_pixels(env, lock.bitmap);
    backend_.release(env, lock.bitmap);
    return finish(guest == nullptr ? -1 : result);
}

}  // namespace zb
