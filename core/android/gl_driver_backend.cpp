#include "gl_driver_backend.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

namespace zb {

void GlDriverBackend::set_error(GLenum error) {
    if (pending_error_ == GL_NO_ERROR) pending_error_ = error;
}

GLenum GlDriverBackend::glGetError(void) {
    if (pending_error_ != GL_NO_ERROR) {
        const GLenum error = pending_error_;
        pending_error_ = GL_NO_ERROR;
        return error;
    }
    return ::glGetError();
}

std::uint64_t GlDriverBackend::invoke(const char*, std::initializer_list<std::uint64_t>) {
    // Every typed virtual is overridden above; the portable invoke() seam (used by MockGles in
    // host tests) is never reached in production.
    return 0;
}

bool gl_egl_context_current() {
    return eglGetCurrentContext() != EGL_NO_CONTEXT;
}

}  // namespace zb
