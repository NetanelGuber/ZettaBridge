#include "zb/host_gl.h"

#include <cstdlib>
#include <cstring>

#include "zb/gl_hostcalls.h"
#include "zb/log.h"

namespace zb {

HostGl::Call::Call(HostGl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
    // Capture arguments before installing the default void/zero result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostGl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address = static_cast<std::uint64_t>(thread_.regs()[13]) + 4u * (position - 4u);
    if (address > UINT32_MAX) {
        fail(kGlInvalidValue, "argument address overflowed the guest stack");
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime().memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        fail(kGlInvalidValue, "argument is not on a readable guest stack");
        return 0;
    }
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

void HostGl::Call::fail(GLenum error, const char* reason) {
    if (!valid_) return;
    valid_ = false;
    host_.reject(*this, error, reason);
}

void HostGl::reject(Call& call, GLenum error, const char* reason) {
    const char* name = call.index() < kGlHostCalls.size() ? kGlHostCalls[call.index()].name : "?";
    log("GLES %s rejected: %s", name, reason);
    backend_.set_error(error);
}

#define ZB_GL_MANUAL(name) bool zbgl_manual_##name(HostGl& host, HostGl::Call& call);
#include "gen/gl_manual.inc"
#undef ZB_GL_MANUAL

#include "gen/gl_dispatch.inc"

#define ZB_GL_MANUAL(name)                                                        \
    bool zbgl_manual_##name(HostGl& host, HostGl::Call& call) {                   \
        host.reject(call, kGlInvalidOperation, #name " is not implemented yet"); \
        return true;                                                              \
    }
#include "gen/gl_manual.inc"
#undef ZB_GL_MANUAL

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index > kGlHostCallLast) return false;
    Call call(*this, thread, index);
    if (dispatch(call)) return true;
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

}  // namespace zb
