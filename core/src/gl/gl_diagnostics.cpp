// GL visibility diagnostics for the runtime report. The render loop runs with no GL error but
// shows a black screen, and glGetError does not catch shader failures, off-screen framebuffers,
// empty viewports or bad float data. After each accepted GL host call this records a few facts
// (and queries the driver a few times) into RuntimeReport's "gl-<key>" lines.

#include "gl/gl_diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "zb/gl_hostcalls.h"
#include "zb/runtime_report.h"

namespace zb {
namespace {

constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kFramebufferBinding = 0x8CA6;
constexpr GLenum kViewport = 0x0BA2;
constexpr GLenum kScissorBox = 0x0C10;
constexpr GLenum kCurrentProgram = 0x8B8D;
constexpr GLenum kArrayBufferBinding = 0x8894;
constexpr GLenum kColorWritemask = 0x0C23;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kDepthTest = 0x0B71;
constexpr GLenum kBlend = 0x0BE2;
constexpr GLenum kCullFace = 0x0B44;
constexpr GLenum kStencilTest = 0x0B90;
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kUnsignedByte = 0x1401;

// Draw numbers at which the state and a 3x3 pixel grid are sampled after the draw.
constexpr std::uint64_t kSampleDraws[] = {1, 300, 3000, 30000};

struct State {
    std::mutex mutex;
    std::uint64_t shaders_ok = 0;
    std::uint64_t shaders_failed = 0;
    std::uint64_t programs_ok = 0;
    std::uint64_t programs_failed = 0;
    std::uint64_t draws = 0;
    std::uint64_t empty_draws = 0;
    std::uint64_t offscreen_draws = 0;
    std::uint64_t clears = 0;
    std::uint64_t fb_binds_nonzero = 0;
    std::uint64_t tex_images = 0;
    std::uint64_t attrib_pointers = 0;
    std::uint64_t buffer_datas = 0;
    std::uint64_t matrices = 0;
    std::uint64_t use_programs = 0;
    GLuint bound_framebuffer = 0;
};

State& state() {
    static State* s = new State();
    return *s;
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

void detail(const char* key, const std::string& value, bool overwrite = false) {
    runtime_report().note_gl_detail(key, value, overwrite);
}

std::string shader_text(GlBackend& gl, GLuint object, bool source) {
    char buffer[400] = {};
    GLsizei length = 0;
    if (source) {
        gl.glGetShaderSource(object, sizeof buffer, &length, buffer);
    } else {
        gl.glGetShaderInfoLog(object, sizeof buffer, &length, buffer);
    }
    if (length < 0 || length >= static_cast<GLsizei>(sizeof buffer)) length = 0;
    return std::string(buffer, static_cast<std::size_t>(length));
}

std::string float_words(HostGl& host, std::uint32_t address, std::uint32_t words) {
    if (address == 0) return "(null)";
    const std::uint8_t* data = host.runtime().memory().host_ptr(address, 4ull * words, kPageRead);
    if (data == nullptr) return format("(unreadable 0x%x)", address);
    std::string out;
    for (std::uint32_t i = 0; i < words; ++i) {
        float value;
        std::memcpy(&value, data + 4 * i, 4);
        out += format(i == 0 ? "%g" : " %g", static_cast<double>(value));
    }
    return out;
}

void sample(HostGl& host, std::uint64_t draw, GLenum mode, GLsizei count) {
    GlBackend& gl = host.backend();
    GLint framebuffer = -1, program = -1, array_buffer = -1;
    GLint viewport[4] = {-1, -1, -1, -1};
    GLint scissor[4] = {-1, -1, -1, -1};
    GLboolean mask[4] = {9, 9, 9, 9};
    gl.glGetIntegerv(kFramebufferBinding, &framebuffer);
    gl.glGetIntegerv(kCurrentProgram, &program);
    gl.glGetIntegerv(kArrayBufferBinding, &array_buffer);
    gl.glGetIntegerv(kViewport, viewport);
    gl.glGetIntegerv(kScissorBox, scissor);
    gl.glGetBooleanv(kColorWritemask, mask);
    const GLenum fb_status = gl.glCheckFramebufferStatus(kFramebuffer);
    std::string text = format(
        "mode=0x%x count=%d fb=%d fb-status=0x%x program=%d array-buffer=%d viewport=%d,%d,%dx%d "
        "scissor-test=%d box=%d,%d,%dx%d depth=%d blend=%d cull=%d stencil=%d colormask=%d%d%d%d",
        mode, count, framebuffer, fb_status, program, array_buffer, viewport[0], viewport[1],
        viewport[2], viewport[3], gl.glIsEnabled(kScissorTest), scissor[0], scissor[1], scissor[2],
        scissor[3], gl.glIsEnabled(kDepthTest), gl.glIsEnabled(kBlend), gl.glIsEnabled(kCullFace),
        gl.glIsEnabled(kStencilTest), mask[0], mask[1], mask[2], mask[3]);
    const std::string key = "sample-draw-" + std::to_string(draw);
    detail(key.c_str(), text);

    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    std::string pixels;
    for (int row = 1; row <= 3; ++row) {
        for (int column = 1; column <= 3; ++column) {
            const GLint x = viewport[0] + viewport[2] * column / 4;
            const GLint y = viewport[1] + viewport[3] * row / 4;
            std::uint8_t rgba[4] = {};
            gl.glReadPixels(x, y, 1, 1, kRgba, kUnsignedByte, rgba);
            pixels += format("%s%02x%02x%02x%02x", pixels.empty() ? "" : " ", rgba[0], rgba[1],
                             rgba[2], rgba[3]);
        }
    }
    const std::string pixel_key = key + "-pixels";
    detail(pixel_key.c_str(), pixels);
}

}  // namespace

void gl_diagnose(HostGl& host, HostGl::Call& call) {
    GlBackend& gl = host.backend();
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    switch (call.index()) {
    case ZB_GL_HC_glCompileShader: {
        const GLuint shader = call.arg(0);
        GLint status = -1;
        gl.glGetShaderiv(shader, kCompileStatus, &status);
        if (status == 1) {
            if (++s.shaders_ok == 1) detail("first-shader-source", shader_text(gl, shader, true));
        } else if (++s.shaders_failed == 1) {
            detail("first-shader-failure", format("status=%d log=", status) + shader_text(gl, shader, false));
            detail("first-shader-failure-source", shader_text(gl, shader, true));
        }
        detail("shaders", format("ok=%llu failed=%llu", (unsigned long long)s.shaders_ok,
                                 (unsigned long long)s.shaders_failed), true);
        break;
    }
    case ZB_GL_HC_glLinkProgram: {
        const GLuint program = call.arg(0);
        GLint status = -1;
        gl.glGetProgramiv(program, kLinkStatus, &status);
        if (status == 1) {
            ++s.programs_ok;
        } else if (++s.programs_failed == 1) {
            char log[400] = {};
            GLsizei length = 0;
            gl.glGetProgramInfoLog(program, sizeof log, &length, log);
            if (length < 0 || length >= static_cast<GLsizei>(sizeof log)) length = 0;
            detail("first-link-failure",
                   format("status=%d log=", status) + std::string(log, static_cast<std::size_t>(length)));
        }
        detail("programs", format("ok=%llu failed=%llu", (unsigned long long)s.programs_ok,
                                  (unsigned long long)s.programs_failed), true);
        break;
    }
    case ZB_GL_HC_glUseProgram:
        if (++s.use_programs == 1) detail("first-use-program", format("%u", call.arg(0)));
        break;
    case ZB_GL_HC_glDrawArrays:
    case ZB_GL_HC_glDrawElements: {
        const bool arrays = call.index() == ZB_GL_HC_glDrawArrays;
        const GLenum mode = call.arg(0);
        const GLsizei count = static_cast<GLsizei>(call.arg(arrays ? 2 : 1));
        const std::uint64_t draw = ++s.draws;
        if (count <= 0) ++s.empty_draws;
        if (s.bound_framebuffer != 0) ++s.offscreen_draws;
        if (draw == 1) {
            detail("first-draw-call", arrays ? format("glDrawArrays first=%d count=%d", static_cast<GLint>(call.arg(1)), count)
                                             : format("glDrawElements count=%d type=0x%x indices=0x%x",
                                                      count, call.arg(2), call.arg(3)));
        }
        for (std::uint64_t at : kSampleDraws) {
            if (draw == at) sample(host, draw, mode, count);
        }
        if (draw == 1 || draw % 256 == 0) {
            detail("draws", format("total=%llu empty=%llu offscreen=%llu clears=%llu",
                                   (unsigned long long)s.draws, (unsigned long long)s.empty_draws,
                                   (unsigned long long)s.offscreen_draws, (unsigned long long)s.clears),
                   true);
        }
        break;
    }
    case ZB_GL_HC_glClear:
        if (++s.clears == 1) detail("first-clear-mask", format("0x%x", call.arg(0)));
        break;
    case ZB_GL_HC_glClearColor:
        detail("last-clear-color", format("%g %g %g %g", static_cast<double>(call.scalar<GLfloat>(0)),
                                          static_cast<double>(call.scalar<GLfloat>(1)),
                                          static_cast<double>(call.scalar<GLfloat>(2)),
                                          static_cast<double>(call.scalar<GLfloat>(3))), true);
        break;
    case ZB_GL_HC_glBindFramebuffer:
        s.bound_framebuffer = call.arg(1);
        if (s.bound_framebuffer != 0 && ++s.fb_binds_nonzero <= 4) {
            detail("framebuffer-binds-nonzero", format("%llu last=%u", (unsigned long long)s.fb_binds_nonzero,
                                                      s.bound_framebuffer), true);
        }
        break;
    case ZB_GL_HC_glScissor:
        detail("last-scissor", format("%d,%d,%dx%d", static_cast<GLint>(call.arg(0)), static_cast<GLint>(call.arg(1)),
                                      static_cast<GLint>(call.arg(2)), static_cast<GLint>(call.arg(3))), true);
        break;
    case ZB_GL_HC_glTexImage2D:
        if (++s.tex_images <= 2) {
            const std::string key = "teximage-" + std::to_string(s.tex_images);
            detail(key.c_str(), format("target=0x%x level=%d internal=0x%x %dx%d format=0x%x type=0x%x pixels=0x%x",
                                       call.arg(0), static_cast<GLint>(call.arg(1)), call.arg(2),
                                       static_cast<GLint>(call.arg(3)), static_cast<GLint>(call.arg(4)),
                                       call.arg(6), call.arg(7), call.arg(8)));
        }
        break;
    case ZB_GL_HC_glVertexAttribPointer:
        if (++s.attrib_pointers <= 3) {
            const std::string key = "attrib-pointer-" + std::to_string(s.attrib_pointers);
            detail(key.c_str(), format("index=%u size=%d type=0x%x normalized=%u stride=%d pointer=0x%x",
                                       call.arg(0), static_cast<GLint>(call.arg(1)), call.arg(2),
                                       call.arg(3) & 0xff, static_cast<GLint>(call.arg(4)), call.arg(5)));
        }
        break;
    case ZB_GL_HC_glBufferData:
        if (++s.buffer_datas <= 2) {
            const std::string key = "buffer-data-" + std::to_string(s.buffer_datas);
            const std::uint32_t size = call.arg(1);
            detail(key.c_str(), format("target=0x%x size=%u usage=0x%x floats=", call.arg(0), size, call.arg(3)) +
                                    float_words(host, call.arg(2), size >= 32 ? 8 : size / 4));
        }
        break;
    case ZB_GL_HC_glUniformMatrix4fv:
        if (++s.matrices == 1) {
            detail("first-matrix4", format("location=%d count=%d transpose=%u values=",
                                           static_cast<GLint>(call.arg(0)), static_cast<GLint>(call.arg(1)),
                                           call.arg(2) & 0xff) +
                                        float_words(host, call.arg(3), 16));
        }
        break;
    default:
        break;
    }
}

}  // namespace zb
