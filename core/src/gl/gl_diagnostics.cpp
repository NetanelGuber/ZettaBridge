// GL visibility diagnostics for the runtime report. The render loop runs with no GL error but
// shows a black screen, and glGetError does not catch shader failures, off-screen framebuffers,
// empty viewports or bad float data. After each accepted GL host call this records a few facts
// (and queries the driver a few times) into RuntimeReport's "gl-<key>" lines.

#include "gl/gl_diagnostics.h"

#include <algorithm>
#include <atomic>
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
constexpr GLenum kFloat = 0x1406;
constexpr GLenum kTextureBinding2d = 0x8069;
constexpr GLenum kActiveTexture = 0x84E0;
constexpr GLenum kTexture2d = 0x0DE1;
constexpr GLenum kMagFilter = 0x2800;
constexpr GLenum kMinFilter = 0x2801;
constexpr GLenum kWrapS = 0x2802;
constexpr GLenum kWrapT = 0x2803;
constexpr GLenum kAttribEnabled = 0x8622;

struct Attrib {
    bool set = false;
    GLint size = 0;
    GLenum type = 0;
    GLint stride = 0;
    std::uint32_t pointer = 0;
};

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
    std::uint64_t tex_uploads = 0;
    std::uint64_t tex_sub_images = 0;
    std::uint64_t tex_parameters = 0;
    std::uint64_t mipmaps = 0;
    std::uint64_t uniform_ints = 0;
    std::uint64_t pixel_stores = 0;
    std::uint64_t shader_sources = 0;
    std::uint64_t clear_samples = 0;
    GLuint bound_framebuffer = 0;
    Attrib attribs[8];
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

    GLint texture = -1, active = -1;
    GLint params[4] = {-1, -1, -1, -1};
    gl.glGetIntegerv(kTextureBinding2d, &texture);
    gl.glGetIntegerv(kActiveTexture, &active);
    gl.glGetTexParameteriv(kTexture2d, kMinFilter, &params[0]);
    gl.glGetTexParameteriv(kTexture2d, kMagFilter, &params[1]);
    gl.glGetTexParameteriv(kTexture2d, kWrapS, &params[2]);
    gl.glGetTexParameteriv(kTexture2d, kWrapT, &params[3]);
    const std::string texture_key = key + "-texture";
    detail(texture_key.c_str(), format("active=0x%x bound=%d min=0x%x mag=0x%x wrap-s=0x%x wrap-t=0x%x",
                                       active, texture, params[0], params[1], params[2], params[3]));

    std::string vertices;
    for (GLuint index = 0; index < 8; ++index) {
        GLint enabled = 0;
        gl.glGetVertexAttribiv(index, kAttribEnabled, &enabled);
        if (!enabled) continue;
        const Attrib& attrib = state().attribs[index];
        vertices += format("%sa%u:", vertices.empty() ? "" : " | ", index);
        if (!attrib.set) {
            vertices += "(no pointer)";
            continue;
        }
        const GLint step = attrib.stride != 0 ? attrib.stride
                                              : attrib.size * (attrib.type == kFloat ? 4 : 1);
        for (int vertex = 0; vertex < 3; ++vertex) {
            const std::uint32_t at = attrib.pointer + static_cast<std::uint32_t>(step * vertex);
            if (attrib.type == kFloat) {
                vertices += " [" + float_words(host, at, static_cast<std::uint32_t>(attrib.size)) + "]";
            } else {
                const std::uint8_t* bytes = host.runtime().memory().host_ptr(
                    at, static_cast<std::uint64_t>(attrib.size), kPageRead);
                vertices += " [";
                for (GLint i = 0; bytes != nullptr && i < attrib.size; ++i) {
                    vertices += format(i == 0 ? "%u" : " %u", bytes[i]);
                }
                vertices += "]";
            }
        }
    }
    const std::string vertex_key = key + "-vertices";
    detail(vertex_key.c_str(), vertices);

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

std::atomic<bool> g_enabled{false};

}  // namespace

void enable_gl_diagnostics() { g_enabled.store(true, std::memory_order_relaxed); }

bool gl_diagnostics_enabled() { return g_enabled.load(std::memory_order_relaxed); }

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
            if (++s.shaders_ok <= 4) {
                const std::string key = "shader-source-" + std::to_string(s.shaders_ok);
                detail(key.c_str(), shader_text(gl, shader, true));
            }
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
        if ((s.clears == 1 || s.clears == 200) && s.bound_framebuffer == 0) {
            std::uint8_t rgba[4] = {};
            GLint viewport[4] = {0, 0, 0, 0};
            gl.glGetIntegerv(kViewport, viewport);
            gl.glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2, 1, 1, kRgba,
                            kUnsignedByte, rgba);
            const std::string key = "clear-" + std::to_string(s.clears) + "-center-pixel";
            detail(key.c_str(), format("%02x%02x%02x%02x", rgba[0], rgba[1], rgba[2], rgba[3]));
        }
        break;
    case ZB_GL_HC_glShaderSource:
        ++s.shader_sources;
        break;
    case ZB_GL_HC_glTexSubImage2D:
        if (++s.tex_sub_images == 1) {
            detail("first-texsubimage", format("level=%d at=%d,%d %dx%d format=0x%x type=0x%x pixels=0x%x",
                                               static_cast<GLint>(call.arg(1)), static_cast<GLint>(call.arg(2)),
                                               static_cast<GLint>(call.arg(3)), static_cast<GLint>(call.arg(4)),
                                               static_cast<GLint>(call.arg(5)), call.arg(6), call.arg(7), call.arg(8)));
        }
        detail("texsubimages", std::to_string(s.tex_sub_images), true);
        break;
    case ZB_GL_HC_glTexParameteri:
    case ZB_GL_HC_glTexParameterf: {
        const bool is_float = call.index() == ZB_GL_HC_glTexParameterf;
        if (++s.tex_parameters <= 8) {
            const std::string key = "texparameter-" + std::to_string(s.tex_parameters);
            detail(key.c_str(), is_float ? format("f target=0x%x pname=0x%x param=%g", call.arg(0), call.arg(1),
                                                  static_cast<double>(call.scalar<GLfloat>(2)))
                                         : format("i target=0x%x pname=0x%x param=0x%x", call.arg(0), call.arg(1),
                                                  call.arg(2)));
        }
        break;
    }
    case ZB_GL_HC_glGenerateMipmap:
        detail("generate-mipmap", std::to_string(++s.mipmaps), true);
        break;
    case ZB_GL_HC_glUniform1i:
        if (++s.uniform_ints <= 3) {
            const std::string key = "uniform1i-" + std::to_string(s.uniform_ints);
            detail(key.c_str(), format("location=%d value=%d", static_cast<GLint>(call.arg(0)),
                                       static_cast<GLint>(call.arg(1))));
        }
        break;
    case ZB_GL_HC_glPixelStorei:
        if (++s.pixel_stores <= 3) {
            const std::string key = "pixelstore-" + std::to_string(s.pixel_stores);
            detail(key.c_str(), format("pname=0x%x param=%d", call.arg(0), static_cast<GLint>(call.arg(1))));
        }
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
        if (call.arg(8) != 0 && ++s.tex_uploads <= 3) {
            const GLint width = static_cast<GLint>(call.arg(3));
            const GLint height = static_cast<GLint>(call.arg(4));
            const std::uint64_t bytes = (width > 0 && height > 0 && call.arg(7) == kUnsignedByte)
                                            ? std::uint64_t(width) * std::uint64_t(height) *
                                                  (call.arg(6) == kRgba ? 4 : 3)
                                            : 0;
            const std::uint8_t* data = bytes != 0 ? host.runtime().memory().host_ptr(call.arg(8), bytes, kPageRead)
                                                  : nullptr;
            std::uint64_t nonzero = 0;
            for (std::uint64_t i = 0; data != nullptr && i < bytes; ++i) nonzero += data[i] != 0;
            std::string head;
            for (std::uint64_t i = 0; data != nullptr && i < std::min<std::uint64_t>(bytes, 16); ++i) {
                head += format("%02x", data[i]);
            }
            const std::string key = "texupload-" + std::to_string(s.tex_uploads);
            detail(key.c_str(), format("level=%d internal=0x%x %dx%d format=0x%x type=0x%x pixels=0x%x "
                                       "readable=%d nonzero-bytes=%llu/%llu head=",
                                       static_cast<GLint>(call.arg(1)), call.arg(2), width, height, call.arg(6),
                                       call.arg(7), call.arg(8), data != nullptr ? 1 : 0,
                                       (unsigned long long)nonzero, (unsigned long long)bytes) + head);
        }
        detail("teximages", std::to_string(s.tex_images + 1), true);
        if (++s.tex_images <= 2) {
            const std::string key = "teximage-" + std::to_string(s.tex_images);
            detail(key.c_str(), format("target=0x%x level=%d internal=0x%x %dx%d format=0x%x type=0x%x pixels=0x%x",
                                       call.arg(0), static_cast<GLint>(call.arg(1)), call.arg(2),
                                       static_cast<GLint>(call.arg(3)), static_cast<GLint>(call.arg(4)),
                                       call.arg(6), call.arg(7), call.arg(8)));
        }
        break;
    case ZB_GL_HC_glVertexAttribPointer:
        if (call.arg(0) < 8) {
            Attrib& attrib = s.attribs[call.arg(0)];
            attrib.set = true;
            attrib.size = static_cast<GLint>(call.arg(1));
            attrib.type = call.arg(2);
            attrib.stride = static_cast<GLint>(call.arg(4));
            attrib.pointer = call.arg(5);
        }
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
