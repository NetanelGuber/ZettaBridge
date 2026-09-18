#pragma once

#include <GLES3/gl3.h>

#include "zb/gl_backend.h"

namespace zb {

// Phase 5 Task 8: a thin passthrough GlBackend that calls the real device driver
// (libGLESv2.so). All marshaling (pointer translation, bounds checks, client-array
// materialization) already happened in HostGl before a call reaches here, so every override
// below is a direct forward with no logic of its own. This and gl_driver_backend.cpp are the
// only files in the tree that include GLES3/gl3.h (which includes GLES2/gl2.h) for real.
class GlDriverBackend final : public GlBackend {
public:
    // There is no real API to inject an error into the driver's own error queue, so a rejection
    // from HostGl's bounds checks (a bad guest pointer, never reaching the driver) is queued here
    // instead and returned by the next glGetError(), ahead of whatever the driver itself queued.
    // GL error state is sticky until read: only the first rejection between two glGetError()
    // calls is kept.
    void set_error(GLenum error) override;
    GLenum glGetError(void) override;

    // Generated from core/include/zb/gl_backend.h by a one-off script (see
    // docs/superpowers/plans/2026-09-16-phase5-gles.md, Task 8): every other GlBackend virtual,
    // forwarding straight to the ::gl* driver entry point of the same name.
#include "gl_driver_backend_overrides.inc"

protected:
    // Never called: every method above is overridden directly, matching the design ("The real
    // GlBackend calls the driver directly").
    std::uint64_t invoke(const char* name, std::initializer_list<std::uint64_t> arguments) override;

private:
    GLenum pending_error_ = 0;  // GL_NO_ERROR
};

// eglGetCurrentContext() != EGL_NO_CONTEXT on the calling thread. Links EGL only for this check,
// called once by HostGl on the first GLES host call (see RuntimeReport's GL section).
bool gl_egl_context_current();

}  // namespace zb
