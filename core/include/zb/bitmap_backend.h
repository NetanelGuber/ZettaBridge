#pragma once

#include <cstdint>

#include "zb/jni_backend.h"

namespace zb {

struct BitmapInfo {
    std::uint32_t width = 0, height = 0, stride = 0;
    std::int32_t format = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(BitmapInfo) == 20);

class BitmapBackend {
public:
    virtual ~BitmapBackend() = default;
    virtual int get_info(JniBackend::Env env, JniBackend::Ref bitmap, BitmapInfo& info) = 0;
    virtual int lock_pixels(JniBackend::Env env, JniBackend::Ref bitmap, void*& pixels) = 0;
    virtual int unlock_pixels(JniBackend::Env env, JniBackend::Ref bitmap) = 0;
    virtual JniBackend::Ref retain(JniBackend::Env env, JniBackend::Ref bitmap) = 0;
    virtual void release(JniBackend::Env env, JniBackend::Ref bitmap) = 0;
    virtual bool same_object(JniBackend::Env env, JniBackend::Ref a, JniBackend::Ref b) = 0;
};

}  // namespace zb
