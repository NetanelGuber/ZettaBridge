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
