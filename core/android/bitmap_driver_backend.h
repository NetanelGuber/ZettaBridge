#pragma once

#include "zb/bitmap_backend.h"

namespace zb {

class AndroidBitmapBackend final : public BitmapBackend {
public:
    int get_info(JniBackend::Env env, JniBackend::Ref bitmap, BitmapInfo& info) override;
    int lock_pixels(JniBackend::Env env, JniBackend::Ref bitmap, void*& pixels) override;
    int unlock_pixels(JniBackend::Env env, JniBackend::Ref bitmap) override;
    JniBackend::Ref retain(JniBackend::Env env, JniBackend::Ref bitmap) override;
    void release(JniBackend::Env env, JniBackend::Ref bitmap) override;
    bool same_object(JniBackend::Env env, JniBackend::Ref a, JniBackend::Ref b) override;
};

}  // namespace zb
