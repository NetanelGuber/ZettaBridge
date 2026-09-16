#include "zb/host_gl.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zb/log.h"

namespace zb {

namespace {

constexpr GLenum kGlPackAlignment = 0x0D05;
constexpr GLenum kGlUnpackAlignment = 0x0CF5;
constexpr GLenum kGlActiveUniforms = 0x8B86;
constexpr GLenum kGlActiveUniformMaxLength = 0x8B87;
constexpr GLenum kGlNumCompressedTextureFormats = 0x86A2;
constexpr GLenum kGlCompressedTextureFormats = 0x86A3;
constexpr GLenum kGlNumShaderBinaryFormats = 0x8DF9;
constexpr GLenum kGlShaderBinaryFormats = 0x8DF8;

struct GlThreadState {
    const HostGl* owner = nullptr;
    GLint pack_alignment = 4;
    GLint unpack_alignment = 4;
    std::unordered_map<GLuint, std::unordered_map<GLint, std::uint64_t>> uniforms;
    std::unordered_map<GLenum, std::uint32_t> strings;
};

thread_local GlThreadState t_state;

GlThreadState& state(const HostGl& host) {
    if (t_state.owner != &host) {
        t_state = {};
        t_state.owner = &host;
    }
    return t_state;
}

std::uint64_t uniform_components(GLenum type) {
    switch (type) {
    case 0x1404:  // GL_INT
    case 0x1406:  // GL_FLOAT
    case 0x8B56:  // GL_BOOL
    case 0x8B5E:  // GL_SAMPLER_2D
    case 0x8B60:  // GL_SAMPLER_CUBE
        return 1;
    case 0x8B50:  // GL_FLOAT_VEC2
    case 0x8B53:  // GL_INT_VEC2
    case 0x8B57:  // GL_BOOL_VEC2
        return 2;
    case 0x8B51:  // GL_FLOAT_VEC3
    case 0x8B54:  // GL_INT_VEC3
    case 0x8B58:  // GL_BOOL_VEC3
        return 3;
    case 0x8B52:  // GL_FLOAT_VEC4
    case 0x8B55:  // GL_INT_VEC4
    case 0x8B59:  // GL_BOOL_VEC4
    case 0x8B5A:  // GL_FLOAT_MAT2
        return 4;
    case 0x8B5B:  // GL_FLOAT_MAT3
        return 9;
    case 0x8B5C:  // GL_FLOAT_MAT4
        return 16;
    default:
        return 0;
    }
}

bool checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

template <typename T>
bool serve_pname(HostGl& host, HostGl::Call& call, void (GlBackend::*function)(GLenum, T*)) {
    const GLenum pname = call.scalar<GLenum>(0);
    const std::uint64_t count = gl_pname_count(host.backend(), pname);
    T* data = call.pointer<T>(1, count, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    (host.backend().*function)(pname, data);
    return true;
}

bool pixel_pointer(HostGl::Call& call, GLenum format, GLenum type, GLsizei width,
                   GLsizei height, GLint alignment, unsigned position,
                   std::uint8_t permission, void*& out) {
    const auto bytes = gl_pixel_bytes(format, type, width, height, alignment);
    if (!bytes) {
        call.fail(kGlInvalidValue, "pixel format, type, dimensions or alignment is invalid");
        return false;
    }
    out = call.pointer<void>(position, *bytes, permission);
    return call.valid();
}

const GLchar* guest_string(HostGl& host, HostGl::Call& call, std::uint32_t address) {
    constexpr std::uint64_t kMaxGuestString = 64u << 20;
    if (address == 0) {
        call.fail(kGlInvalidValue, "string pointer is null");
        return nullptr;
    }
    std::uint64_t scanned = 0;
    while (scanned < kMaxGuestString) {
        const std::uint64_t current = static_cast<std::uint64_t>(address) + scanned;
        if (current >= kGuestSpaceSize) break;
        const std::uint64_t page_left = kPageSize - (current & kPageMask);
        const std::uint64_t chunk = std::min({page_left, kMaxGuestString - scanned,
                                              kGuestSpaceSize - current});
        const std::uint8_t* data = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(current), chunk, kPageRead);
        if (data == nullptr) break;
        if (std::memchr(data, 0, static_cast<std::size_t>(chunk)) != nullptr) {
            return reinterpret_cast<const GLchar*>(host.runtime().memory().base() + address);
        }
        scanned += chunk;
    }
    call.fail(kGlInvalidValue, "string is unreadable or not terminated within 64 MiB");
    return nullptr;
}

}  // namespace

std::uint64_t gl_pname_count(GlBackend& backend, GLenum pname) {
    switch (pname) {
    case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
    case 0x0B70:  // GL_DEPTH_RANGE
    case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
        return 2;
    case 0x8005:  // GL_BLEND_COLOR
    case 0x0C22:  // GL_COLOR_CLEAR_VALUE
    case 0x0C23:  // GL_COLOR_WRITEMASK
    case 0x0C10:  // GL_SCISSOR_BOX
    case 0x0BA2:  // GL_VIEWPORT
        return 4;
    case kGlCompressedTextureFormats:
    case kGlShaderBinaryFormats: {
        GLint count = 0;
        backend.glGetIntegerv(pname == kGlCompressedTextureFormats
                                  ? kGlNumCompressedTextureFormats
                                  : kGlNumShaderBinaryFormats,
                              &count);
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }
    default: {
        static std::mutex mutex;
        static std::unordered_set<GLenum> logged;
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(pname).second) {
            log("GLES pname 0x%x assumed to have one result element", pname);
        }
        return 1;
    }
    }
}

std::optional<std::uint64_t> gl_pixel_bytes(GLenum format, GLenum type, GLsizei width,
                                            GLsizei height, GLint alignment) {
    if (width < 0 || height < 0 || (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) {
        return std::nullopt;
    }
    std::uint64_t bytes_per_pixel = 0;
    if (type == 0x1401) {  // GL_UNSIGNED_BYTE
        switch (format) {
        case 0x1906:  // GL_ALPHA
        case 0x1909:  // GL_LUMINANCE
            bytes_per_pixel = 1;
            break;
        case 0x190A:  // GL_LUMINANCE_ALPHA
            bytes_per_pixel = 2;
            break;
        case 0x1907:  // GL_RGB
            bytes_per_pixel = 3;
            break;
        case 0x1908:  // GL_RGBA
            bytes_per_pixel = 4;
            break;
        default:
            return std::nullopt;
        }
    } else if ((type == 0x8363 && format == 0x1907) ||
               ((type == 0x8033 || type == 0x8034) && format == 0x1908)) {
        bytes_per_pixel = 2;
    } else {
        return std::nullopt;
    }

    std::uint64_t row = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(width), bytes_per_pixel, row)) return std::nullopt;
    if (height == 0 || row == 0) return 0;
    const std::uint64_t align = static_cast<std::uint64_t>(alignment);
    if (row > std::numeric_limits<std::uint64_t>::max() - (align - 1)) return std::nullopt;
    const std::uint64_t stride = (row + align - 1) & ~(align - 1);
    std::uint64_t preceding = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(height - 1), stride, preceding) ||
        preceding > std::numeric_limits<std::uint64_t>::max() - row) {
        return std::nullopt;
    }
    return preceding + row;
}

void HostGl::note_pixel_store(GLenum pname, GLint param) {
    if (param != 1 && param != 2 && param != 4 && param != 8) return;
    GlThreadState& current = state(*this);
    if (pname == kGlPackAlignment) current.pack_alignment = param;
    if (pname == kGlUnpackAlignment) current.unpack_alignment = param;
}

GLint HostGl::pixel_alignment(bool pack) const {
    const GlThreadState& current = state(*this);
    return pack ? current.pack_alignment : current.unpack_alignment;
}

void HostGl::invalidate_uniforms(GLuint program) {
    state(*this).uniforms.erase(program);
}

std::optional<std::uint32_t> HostGl::allocate_guest(std::size_t size) {
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!result || result->r0 == 0) return std::nullopt;
    return result->r0;
}

std::optional<std::uint64_t> HostGl::uniform_elements(GLuint program, GLint location) {
    GlThreadState& current = state(*this);
    auto cached = current.uniforms.find(program);
    if (cached == current.uniforms.end()) {
        std::unordered_map<GLint, std::uint64_t> locations;
        GLint count = 0;
        GLint max_length = 0;
        backend_.glGetProgramiv(program, kGlActiveUniforms, &count);
        backend_.glGetProgramiv(program, kGlActiveUniformMaxLength, &max_length);
        if (count < 0) count = 0;
        max_length = std::clamp(max_length, 1, 1 << 20);
        std::vector<GLchar> name(static_cast<std::size_t>(max_length));
        for (GLint index = 0; index < count; ++index) {
            GLsizei written = 0;
            GLint size = 0;
            GLenum type = 0;
            backend_.glGetActiveUniform(program, static_cast<GLuint>(index), max_length,
                                        &written, &size, &type, name.data());
            if (written < 0 || written >= max_length) continue;
            name[static_cast<std::size_t>(written)] = '\0';
            std::string uniform(name.data(), static_cast<std::size_t>(written));
            if (uniform.ends_with("[0]")) uniform.resize(uniform.size() - 3);
            const GLint uniform_location = backend_.glGetUniformLocation(program, uniform.c_str());
            const std::uint64_t elements = uniform_components(type);
            if (uniform_location >= 0 && elements != 0) locations[uniform_location] = elements;
        }
        cached = current.uniforms.emplace(program, std::move(locations)).first;
    }
    const auto found = cached->second.find(location);
    if (found == cached->second.end()) return std::nullopt;
    return found->second;
}

bool zbgl_manual_glGetBooleanv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetBooleanv);
}

bool zbgl_manual_glGetFloatv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetFloatv);
}

bool zbgl_manual_glGetIntegerv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetIntegerv);
}

bool zbgl_manual_glTexImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint internalformat = call.scalar<GLint>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLint border = call.scalar<GLint>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(call, format, type, width, height, host.pixel_alignment(false),
                       8, kPageRead, pixels)) return true;
    host.backend().glTexImage2D(target, level, internalformat, width, height, border,
                                format, type, pixels);
    return true;
}

bool zbgl_manual_glTexSubImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLsizei width = call.scalar<GLsizei>(4);
    const GLsizei height = call.scalar<GLsizei>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(call, format, type, width, height, host.pixel_alignment(false),
                       8, kPageRead, pixels)) return true;
    host.backend().glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                                   format, type, pixels);
    return true;
}

bool zbgl_manual_glReadPixels(HostGl& host, HostGl::Call& call) {
    const GLint x = call.scalar<GLint>(0);
    const GLint y = call.scalar<GLint>(1);
    const GLsizei width = call.scalar<GLsizei>(2);
    const GLsizei height = call.scalar<GLsizei>(3);
    const GLenum format = call.scalar<GLenum>(4);
    const GLenum type = call.scalar<GLenum>(5);
    void* pixels = nullptr;
    if (!pixel_pointer(call, format, type, width, height, host.pixel_alignment(true),
                       6, kPageRead | kPageWrite, pixels)) return true;
    host.backend().glReadPixels(x, y, width, height, format, type, pixels);
    return true;
}

bool zbgl_manual_glGetUniformfv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLfloat* params = call.pointer<GLfloat>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformfv(program, location, params);
    return true;
}

bool zbgl_manual_glGetUniformiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLint* params = call.pointer<GLint>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformiv(program, location, params);
    return true;
}

bool zbgl_manual_glBindAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLuint index = call.scalar<GLuint>(1);
    const GLchar* name = guest_string(host, call, call.arg(2));
    if (call.valid()) host.backend().glBindAttribLocation(program, index, name);
    return true;
}

bool zbgl_manual_glGetAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetAttribLocation(program, name));
    return true;
}

bool zbgl_manual_glGetUniformLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetUniformLocation(program, name));
    return true;
}

bool zbgl_manual_glGetString(HostGl& host, HostGl::Call& call) {
    const GLenum name = call.scalar<GLenum>(0);
    GlThreadState& current = state(host);
    const auto cached = current.strings.find(name);
    if (cached != current.strings.end()) {
        call.set_result(cached->second);
        return true;
    }
    const GLubyte* source = host.backend().glGetString(name);
    if (source == nullptr) return true;
    constexpr std::size_t kMaxDriverString = 64u << 20;
    const std::size_t length = strnlen(reinterpret_cast<const char*>(source), kMaxDriverString);
    if (length == kMaxDriverString) {
        host.reject(call, kGlInvalidOperation, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    if (!address) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return true;
    }
    std::uint8_t* destination = host.runtime().memory().host_ptr(
        *address, length + 1, kPageRead | kPageWrite);
    if (destination == nullptr) {
        host.reject(call, kGlInvalidOperation, "guest allocator returned an unreadable buffer");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    current.strings[name] = *address;
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glShaderSource(HostGl& host, HostGl::Call& call) {
    const GLuint shader = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    if (count < 0) {
        call.fail(kGlInvalidValue, "shader source count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    const std::uint32_t* guest_sources =
        call.pointer<const std::uint32_t>(2, items, kPageRead);
    const GLint* lengths = call.pointer<const GLint>(3, items, kPageRead);
    if (!call.valid()) return true;

    std::vector<const GLchar*> sources(static_cast<std::size_t>(items));
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::uint32_t address = guest_sources[i];
        if (lengths == nullptr || lengths[i] < 0) {
            sources[i] = guest_string(host, call, address);
        } else if (address == 0) {
            call.fail(kGlInvalidValue, "shader source pointer is null");
        } else {
            const std::uint8_t* source = host.runtime().memory().host_ptr(
                address, static_cast<std::uint64_t>(lengths[i]), kPageRead);
            if (source == nullptr) {
                call.fail(kGlInvalidValue, "shader source range is unreadable");
            } else {
                sources[i] = reinterpret_cast<const GLchar*>(source);
            }
        }
        if (!call.valid()) return true;
    }
    host.backend().glShaderSource(shader, count,
                                  sources.empty() ? nullptr : sources.data(), lengths);
    return true;
}

#define ZB_GL_STUB(name)                                                            \
    bool zbgl_manual_##name(HostGl& host, HostGl::Call& call) {                     \
        host.reject(call, kGlInvalidOperation, #name " is not implemented yet");   \
        return true;                                                                \
    }

ZB_GL_STUB(glDrawArrays)
ZB_GL_STUB(glDrawElements)
ZB_GL_STUB(glGetVertexAttribPointerv)
ZB_GL_STUB(glVertexAttribPointer)

#undef ZB_GL_STUB

}  // namespace zb
