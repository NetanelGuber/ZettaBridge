#include <sys/mman.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_gles.h"
#include "zb/gl_hostcalls.h"
#include "zb/host_gl.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;

void set_words(zb::LibraryRuntime& runtime, zb::GuestThread& thread,
               const std::array<std::uint32_t, 12>& words) {
    for (unsigned i = 0; i < 4; ++i) thread.regs()[i] = words[i];
    thread.regs()[13] = kStack;
    std::memcpy(runtime.memory().base() + kStack, words.data() + 4,
                (words.size() - 4) * sizeof(words[0]));
}

void dispatch_all_pointerless(zb::HostGl& host, zb::GuestThread& thread,
                              MockGles& backend, zb::LibraryRuntime& runtime) {
    const std::array<std::uint32_t, 12> words{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::size_t seen = 0;
    for (const zb::GlHostCallInfo& info : zb::kGlHostCalls) {
        if (info.has_pointer) continue;
        ++seen;
        set_words(runtime, thread, words);
        backend.set_error(0);
        const std::size_t before = backend.calls().size();
        CHECK(host.handle_host_call(info.index, thread));
        // These two have no pointer parameters, but the approved design requires semantic
        // handlers: glDrawArrays materializes client arrays and glGetString copies its return.
        if (std::string(info.name) == "glDrawArrays" || std::string(info.name) == "glGetString") {
            CHECK(backend.calls().size() == before);
            CHECK(backend.error() == zb::kGlInvalidOperation);
        } else {
            CHECK(backend.calls().size() == before + 1);
            CHECK(backend.calls().back().name == info.name);
        }
    }
    CHECK(seen == zb::kGlPointerlessHostCallCount);
}

}  // namespace

int main() {
    CHECK(zb::kGlHostCallCount == 142);
    CHECK(zb::kGlPointerlessHostCallCount == 81);
    CHECK(zb::ZB_GL_HC_glActiveTexture == 0);
    CHECK(zb::ZB_GL_HC_glViewport == 141);

    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    MockGles backend;
    zb::HostGl host(runtime, backend);
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // GLES 2.0 has no 64-bit parameter: the two pointer-sized host types consume one guest
    // word and widen it. Both are signed in the NDK ABI.
    const std::array<std::uint32_t, 12> wide_words{0xFFFFFFFFu, 0x80000000u};
    set_words(runtime, thread, wide_words);
    zb::HostGl::Call widening(host, thread, zb::ZB_GL_HC_glBufferSubData);
    CHECK(widening.scalar<zb::GLintptr>(0) == static_cast<zb::GLintptr>(-1));
    CHECK(widening.scalar<zb::GLsizeiptr>(1) ==
          static_cast<zb::GLsizeiptr>(INT32_MIN));
    widening.set_result(static_cast<zb::GLint>(-7));
    CHECK(thread.regs()[0] == 0xFFFFFFF9u && thread.regs()[1] == 0);

    // Every registry function with no pointer parameter is dispatched. The two semantic calls
    // reach their safe Task 1 stubs; the other 79 reach the typed backend entry point.
    dispatch_all_pointerless(host, thread, backend, runtime);

    // r0-r3 must be captured before HostGl clears r0/r1 for the default result. Eight arguments
    // also prove that argument positions 4-7 are read from the guest stack in word order.
    backend.clear_calls();
    const std::array<std::uint32_t, 12> copy_words{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0, 0, 0, 0};
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCopyTexSubImage2D, thread));
    CHECK(backend.calls().size() == 1);
    CHECK(backend.calls()[0].name == "glCopyTexSubImage2D");
    CHECK(backend.calls()[0].arguments.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) CHECK(backend.calls()[0].arguments[i] == 0x10 + i);
    CHECK(thread.regs()[0] == 0 && thread.regs()[1] == 0);

    // GLfloat arguments retain their 32-bit representation rather than being numerically cast.
    backend.clear_calls();
    const std::array<std::uint32_t, 12> float_words{
        std::bit_cast<std::uint32_t>(1.5f), std::bit_cast<std::uint32_t>(-2.25f),
        std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(8.5f)};
    set_words(runtime, thread, float_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBlendColor, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].arguments.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(backend.calls()[0].arguments[i] == float_words[i]);

    // All pointerless return classes: GLenum, GLuint and GLboolean. GLint and pointer returns
    // belong to semantic pointer/string handlers and safely return zero in Task 1.
    backend.set_result("glCheckFramebufferStatus", 0x8CD5);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCheckFramebufferStatus, thread));
    CHECK(thread.regs()[0] == 0x8CD5 && thread.regs()[1] == 0);

    backend.set_result("glCreateProgram", 0x12345678);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCreateProgram, thread));
    CHECK(thread.regs()[0] == 0x12345678 && thread.regs()[1] == 0);

    backend.set_result("glIsBuffer", 0x101);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glIsBuffer, thread));
    CHECK(thread.regs()[0] == 1 && thread.regs()[1] == 0);

    // Registry len= shapes: literal, named parameter, product, and a parameter declared after
    // its pointer. Each guest address must become base + address without copying.
    backend.clear_calls();
    std::array<std::uint32_t, 12> pointer_words{3, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glVertexAttrib4fv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glVertexAttrib4fv");
    CHECK(backend.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0xFF0));

    backend.clear_calls();
    pointer_words = {2, kData + 0xFF8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDeleteBuffers, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glDeleteBuffers");

    backend.clear_calls();
    pointer_words = {7, 2, 0, kData + 0xFB8};  // 2 * 9 floats ends at the page boundary.
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUniformMatrix3fv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glUniformMatrix3fv");

    backend.clear_calls();
    pointer_words = {2, kData, 0x8DF8, kData + 0x100, 5};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glShaderBinary, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glShaderBinary");
    CHECK(backend.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData));
    CHECK(backend.calls()[0].arguments[3] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x100));

    // Task 3 helpers: pname vector widths, padded pixel rows and lazy uniform sizing.
    backend.set_integer(0x86A2, 3);  // GL_NUM_COMPRESSED_TEXTURE_FORMATS
    CHECK(zb::gl_pname_count(backend, 0x0B21) == 1);  // GL_LINE_WIDTH
    CHECK(zb::gl_pname_count(backend, 0x846E) == 2);  // GL_ALIASED_LINE_WIDTH_RANGE
    CHECK(zb::gl_pname_count(backend, 0x0C22) == 4);  // GL_COLOR_CLEAR_VALUE
    CHECK(zb::gl_pname_count(backend, 0x86A3) == 3);  // GL_COMPRESSED_TEXTURE_FORMATS

    CHECK(zb::gl_pixel_bytes(0x1907, 0x1401, 1, 2, 4) == 7);   // RGB/U8: 3 + pad + 3
    CHECK(zb::gl_pixel_bytes(0x1907, 0x8363, 3, 2, 8) == 14);  // RGB/565: 6 + pad + 6
    CHECK(zb::gl_pixel_bytes(0x1908, 0x1401, 2, 2, 1) == 16);  // RGBA/U8
    CHECK(!zb::gl_pixel_bytes(0x1907, 0x8033, 1, 1, 4));       // RGB/4444 is invalid
    struct PixelCase {
        std::uint32_t format;
        std::uint32_t type;
        std::uint64_t bytes_per_pixel;
    };
    const PixelCase pixel_cases[] = {
        {0x1906, 0x1401, 1}, {0x1909, 0x1401, 1}, {0x190A, 0x1401, 2},
        {0x1907, 0x1401, 3}, {0x1908, 0x1401, 4}, {0x1907, 0x8363, 2},
        {0x1908, 0x8033, 2}, {0x1908, 0x8034, 2},
    };
    for (const PixelCase& pixel : pixel_cases) {
        for (const std::uint64_t alignment : {1u, 2u, 4u, 8u}) {
            const std::uint64_t row = 3 * pixel.bytes_per_pixel;
            const std::uint64_t stride = (row + alignment - 1) & ~(alignment - 1);
            CHECK(zb::gl_pixel_bytes(pixel.format, pixel.type, 3, 2,
                                     static_cast<std::int32_t>(alignment)) == stride + row);
        }
    }

    // PixelStore state selects row alignment for the semantic texture handler.
    pointer_words = {0x0CF5, 8};  // GL_UNPACK_ALIGNMENT
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glPixelStorei, thread));
    backend.clear_calls();
    pointer_words = {0x0DE1, 0, 0x1907, 1, 2, 0, 0x1907, 0x1401, kData + 0xFF5};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glTexImage2D, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glTexImage2D");

    // A one-element COMPSIZE(pname) entry is generated rather than left as a stub.
    backend.clear_calls();
    pointer_words = {3, 0x8B81, kData + 0xFFC};  // GL_COMPILE_STATUS
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetShaderiv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glGetShaderiv");

    backend.set_active_uniforms(7, {{"uColor", 1, 0x8B52, 5}});  // GL_FLOAT_VEC4
    backend.clear_calls();
    pointer_words = {7, 5, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(!backend.calls().empty() && backend.calls().back().name == "glGetUniformfv");

    backend.set_active_uniforms(7, {{"uScalar", 1, 0x1406, 9}});  // GL_FLOAT
    pointer_words = {7};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glLinkProgram, thread));
    backend.clear_calls();
    pointer_words = {7, 9, kData + 0xFFC};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(!backend.calls().empty() && backend.calls().back().name == "glGetUniformfv");

    backend.clear_calls();
    backend.set_error(0);
    pointer_words = {7, 99, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(backend.error() == zb::kGlInvalidOperation);
    CHECK(backend.calls().empty() || backend.calls().back().name != "glGetUniformfv");

    backend.set_error(0);
    set_words(runtime, thread, copy_words);
    const std::size_t before_manual = backend.calls().size();
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetAttribLocation, thread));
    CHECK(backend.calls().size() == before_manual);
    CHECK(backend.error() == zb::kGlInvalidOperation);
    CHECK(thread.regs()[0] == 0 && thread.regs()[1] == 0);

    // The asset range is deliberately not swallowed by HostGl.
    CHECK(!host.handle_host_call(142, thread));
    CHECK(!host.handle_host_call(UINT32_MAX, thread));

    std::puts("gles_marshal_test PASS");
    return 0;
}
