#include "looper_driver_backend.h"

#include <android/looper.h>

#include <cstdint>

namespace zb {

namespace {

ALooper* L(std::uint64_t looper) {
    return reinterpret_cast<ALooper*>(static_cast<std::uintptr_t>(looper));
}

std::uint64_t H(ALooper* looper) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(looper));
}

}  // namespace

std::uint64_t AndroidLooperDriverBackend::prepare(int options) {
    return H(ALooper_prepare(options));
}

std::uint64_t AndroidLooperDriverBackend::for_thread() {
    return H(ALooper_forThread());
}

void AndroidLooperDriverBackend::acquire(std::uint64_t looper) {
    if (looper != 0) ALooper_acquire(L(looper));
}

void AndroidLooperDriverBackend::release(std::uint64_t looper) {
    if (looper != 0) ALooper_release(L(looper));
}

int AndroidLooperDriverBackend::add_fd(std::uint64_t looper, int fd, int ident, int events,
                                       Callback callback, void* data) {
    if (looper == 0 || callback == nullptr) return -1;
    // The guest's identifier is meaningless here: nothing polls this looper for us, so the fd is
    // always registered with a callback, and ALooper requires ALOOPER_POLL_CALLBACK for those.
    (void)ident;
    return ALooper_addFd(L(looper), fd, ALOOPER_POLL_CALLBACK, events, callback, data);
}

int AndroidLooperDriverBackend::remove_fd(std::uint64_t looper, int fd) {
    if (looper == 0) return -1;
    return ALooper_removeFd(L(looper), fd);
}

void AndroidLooperDriverBackend::wake(std::uint64_t looper) {
    if (looper != 0) ALooper_wake(L(looper));
}

}  // namespace zb
