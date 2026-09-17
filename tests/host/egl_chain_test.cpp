// Proves that GuestJniEngine chains an EglBackend's HostEgl and a NativeWindowBackend's
// HostNativeWindow into the same graph as HostGl/HostAssets/HostJni (Phase 7a Task 6): an egl*
// and an ANativeWindow_* host call both reach their handler, an unknown window handle is
// rejected without reaching the backend, and the engine leaves host_egl()/host_native_window()
// null when no backend is supplied.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_egl.h"
#include "mock_jvm.h"
#include "mock_native_window.h"
#include "zb/egl_backend.h"
#include "zb/egl_hostcalls.h"
#include "zb/proxy_runtime.h"
#include "zb/window_hostcalls.h"

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode = argv[1];
    CHECK(mode == "no-backend" || mode == "with-backend");

    if (mode == "no-backend") {
        // Neither backend: the engine still works, host_egl()/host_native_window() are null.
        auto* vm = new zb::mock::MockJvm();
        auto* engine = new zb::GuestJniEngine(*vm);
        CHECK(engine->host_egl() == nullptr);
        CHECK(engine->host_native_window() == nullptr);
        std::puts("egl_chain_test no-backend PASS");
        std::fflush(stdout);
        std::_Exit(0);
    }

    // With both backends: host_egl()/host_native_window() are wired to them, and an egl* and an
    // ANativeWindow_* host-call index reach their mock backends through the chained handler.
    auto* vm = new zb::mock::MockJvm();
    auto* egl_backend = new MockEgl();
    auto* window_backend = new MockNativeWindow();
    egl_backend->set_result("eglGetError", zb::kEglSuccess);
    auto* engine = new zb::GuestJniEngine(*vm, /*gl_backend=*/nullptr, /*egl_context_probe=*/{},
                                          /*asset_backend=*/nullptr, egl_backend, window_backend);
    CHECK(engine->host_egl() != nullptr);
    CHECK(engine->host_native_window() != nullptr);
    CHECK(&engine->host_egl()->backend() == egl_backend);

    zb::LibraryRuntime& runtime = engine->runtime();
    CHECK(runtime.memory().map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs()[13] = 0x10000;

    // eglGetError reaches the mock EGL driver through the chained HostEgl.
    thread.regs()[0] = 0;
    thread.regs()[1] = 0;
    thread.regs()[2] = 0;
    thread.regs()[3] = 0;
    CHECK(engine->host_egl()->handle_host_call(zb::ZB_EGL_HC_eglGetError, thread));
    CHECK(thread.regs()[0] == static_cast<std::uint32_t>(zb::kEglSuccess));

    // ANativeWindow_getWidth on an unknown handle reaches HostNativeWindow, returns -1 and never
    // touches the backend; no crash.
    thread.regs()[0] = 0xdeadbeef;
    CHECK(engine->host_native_window()->handle_host_call(zb::ZB_WINDOW_HC_ANativeWindow_getWidth, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == -1);

    // A JNI-range index is not an EGL or window call; both decline it so the JNI half of the
    // chain gets a turn.
    CHECK(!engine->host_egl()->handle_host_call(0xFC00, thread));
    CHECK(!engine->host_native_window()->handle_host_call(0xFC00, thread));

    std::puts("egl_chain_test with-backend PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
