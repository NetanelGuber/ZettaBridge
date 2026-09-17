#include "zb/host_gl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "gl/gl_diagnostics.h"
#include "zb/gl_hostcalls.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

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

#include "gen/gl_manual.inc"

#include "gen/gl_dispatch.inc"

namespace {

bool gl_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ZB_GL_TRACE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

}  // namespace

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index > kGlHostCallLast) return false;
    const char* name = index < kGlHostCalls.size() ? kGlHostCalls[index].name : "?";
    if (gl_trace_enabled()) {
        log("GLES trace: %s(0x%x, 0x%x, 0x%x, 0x%x)", name, thread.regs()[0], thread.regs()[1],
            thread.regs()[2], thread.regs()[3]);
    }
    runtime_report().note_gl_call(name, static_cast<std::uint64_t>(::syscall(SYS_gettid)));
    if (!egl_context_checked_) {
        egl_context_checked_ = true;
        if (egl_context_probe_) runtime_report().note_gl_egl_context(egl_context_probe_());
    }
    Call call(*this, thread, index);
    if (dispatch(call)) {
        // Visibility diagnostics query the driver, so they run only once the Android runtime
        // enables them; mock-backend tests keep exact call logs.
        if (call.valid() && gl_diagnostics_enabled()) gl_diagnose(*this, call);
        if (call.valid() && std::strcmp(name, "glGetError") == 0) {
            const std::uint32_t error = thread.regs()[0];
            if (error != 0) runtime_report().note_gl_error(name, error);
        }
        return true;
    }
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

}  // namespace zb
