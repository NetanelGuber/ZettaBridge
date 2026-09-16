// RuntimeReport: the bounded diagnostics a device run leaves behind, the report text, the
// observer contract and the atomic file writer. No guest code runs here.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "zb/runtime_report.h"

namespace {

namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Every record is one "key: value" line, which is what makes the report diff-friendly.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

std::string line_with(const std::string& text, const std::string& prefix) {
    for (const std::string& line : lines(text)) {
        if (line.compare(0, prefix.size(), prefix) == 0) return line;
    }
    return {};
}

std::size_t count_with(const std::string& text, const std::string& prefix) {
    std::size_t n = 0;
    for (const std::string& line : lines(text)) {
        if (line.compare(0, prefix.size(), prefix) == 0) ++n;
    }
    return n;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void check_empty_report() {
    zb::RuntimeReport report;
    const std::string text = report.text();
    CHECK(lines(text).at(0) == "zettabridge-runtime-report 1");
    CHECK(line_with(text, "plugin:") == "plugin: (none)");
    CHECK(line_with(text, "proxy-loads:") == "proxy-loads: 0");
    CHECK(line_with(text, "jni-onload-calls:") == "jni-onload-calls: 0");
    CHECK(line_with(text, "registered-natives:") == "registered-natives: 0");
    CHECK(line_with(text, "unimplemented-host-calls:") == "unimplemented-host-calls: 0");
    CHECK(line_with(text, "first-unimplemented:") == "first-unimplemented: (none)");
    CHECK(line_with(text, "guest-exit:") == "guest-exit: (none)");
    CHECK(report.first_unimplemented_host_call().empty());
    CHECK(report.unimplemented_host_calls() == 0);
    CHECK(line_with(text, "gl-calls:") == "gl-calls: 0");
    CHECK(line_with(text, "gl-first-call:") == "gl-first-call: (none)");
    CHECK(line_with(text, "gl-egl-context-current:") == "gl-egl-context-current: (unknown)");
    CHECK(line_with(text, "gl-first-error:") == "gl-first-error: (none)");
    CHECK(report.gl_calls() == 0);
}

void check_gl_section() {
    zb::RuntimeReport report;
    report.note_gl_call("glClear", 4242);
    report.note_gl_call("glDrawArrays", 4242);
    report.note_gl_call("glDrawArrays", 4242);
    CHECK(report.gl_calls() == 3);
    std::string text = report.text();
    CHECK(line_with(text, "gl-calls:") == "gl-calls: 3");
    // Only the first call's name and thread id are kept.
    CHECK(line_with(text, "gl-first-call:") == "gl-first-call: glClear tid=4242");

    // EGL context and error are each recorded once; later notes are ignored.
    report.note_gl_egl_context(true);
    report.note_gl_egl_context(false);
    text = report.text();
    CHECK(line_with(text, "gl-egl-context-current:") == "gl-egl-context-current: yes");

    report.note_gl_error("glGetError", 0x0502);
    report.note_gl_error("glGetError", 0x0501);
    text = report.text();
    CHECK(line_with(text, "gl-first-error:") == "gl-first-error: glGetError 0x0502");

    report.clear();
    text = report.text();
    CHECK(line_with(text, "gl-calls:") == "gl-calls: 0");
    CHECK(line_with(text, "gl-first-call:") == "gl-first-call: (none)");
    CHECK(line_with(text, "gl-egl-context-current:") == "gl-egl-context-current: (unknown)");
    CHECK(line_with(text, "gl-first-error:") == "gl-first-error: (none)");
}

void check_unimplemented_host_calls() {
    zb::RuntimeReport report;
    // The first one in order is the one the user must read, however often the others repeat.
    report.note_unimplemented_host_call(11, "libGLESv2.so", "glCreateProgram");
    for (int i = 0; i < 4; ++i) report.note_unimplemented_host_call(12, "libGLESv2.so", "glShaderSource");
    report.note_unimplemented_host_call(11, "libGLESv2.so", "glCreateProgram");

    CHECK(report.first_unimplemented_host_call() == "libGLESv2.so glCreateProgram");
    CHECK(report.unimplemented_host_calls() == 6);
    const std::string text = report.text();
    CHECK(line_with(text, "first-unimplemented:") == "first-unimplemented: libGLESv2.so glCreateProgram");
    CHECK(line_with(text, "unimplemented-host-calls:") == "unimplemented-host-calls: 6");
    CHECK(line_with(text, "unimplemented-distinct:") == "unimplemented-distinct: 2");
    // Distinct entries keep first-seen order.
    const std::vector<std::string> all = lines(text);
    std::size_t first = 0;
    std::size_t second = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i] == "unimplemented: libGLESv2.so glCreateProgram x2") first = i;
        if (all[i] == "unimplemented: libGLESv2.so glShaderSource x4") second = i;
    }
    CHECK(first != 0 && second != 0 && first < second);
}

void check_host_call_bound() {
    zb::RuntimeReport report;
    const std::size_t distinct = zb::RuntimeReport::kMaxDistinctHostCalls + 5;
    for (std::size_t i = 0; i < distinct; ++i) {
        report.note_unimplemented_host_call(static_cast<std::uint32_t>(i), "libGLESv2.so", "glDrawArrays");
    }
    const std::string text = report.text();
    CHECK(count_with(text, "unimplemented: ") == zb::RuntimeReport::kMaxDistinctHostCalls);
    CHECK(line_with(text, "unimplemented-distinct:") == "unimplemented-distinct: " + std::to_string(distinct));
    CHECK(line_with(text, "unimplemented-more:") == "unimplemented-more: 5");
    CHECK(report.unimplemented_host_calls() == distinct);
    // A null name from an unknown index never crashes the report.
    report.note_unimplemented_host_call(9999, nullptr, nullptr);
    CHECK(contains(report.text(), "unimplemented-distinct: " + std::to_string(distinct + 1)));
}

void check_loads_and_natives() {
    zb::RuntimeReport report;
    report.note_plugin("/data/user/0/com.zettabridge.launcher/files/plugins/com.heyhouser.OrangeRoulette", 16);
    report.note_proxy_loaded("libstd.so", 0x00010006);
    report.note_proxy_loaded("liblime.so", 0x00010004);
    report.note_proxy_failed("libopenal.so", "guest dlopen failed:\ncannot locate symbol");
    report.note_jni_onload("liblime.so", true, 0x00010004);
    report.note_jni_onload("libopenal.so", false, 0);
    for (int i = 0; i < 20; ++i) report.note_registered_native();

    CHECK(report.proxy_loads() == 2);
    CHECK(report.jni_onload_calls() == 2);
    CHECK(report.registered_natives() == 20);
    const std::string text = report.text();
    CHECK(contains(text, "plugin: /data/user/0/com.zettabridge.launcher/files/plugins/"
                         "com.heyhouser.OrangeRoulette targetSdk 16"));
    CHECK(line_with(text, "proxy-loads:") == "proxy-loads: 2");
    CHECK(line_with(text, "proxy-failures:") == "proxy-failures: 1");
    CHECK(contains(text, "proxy-loaded: libstd.so jni=0x00010006"));
    CHECK(contains(text, "proxy-loaded: liblime.so jni=0x00010004"));
    // A multi-line error is folded onto the one record line.
    CHECK(contains(text, "proxy-failed: libopenal.so guest dlopen failed: cannot locate symbol"));
    CHECK(contains(text, "jni-onload: liblime.so ok jni=0x00010004"));
    CHECK(contains(text, "jni-onload: libopenal.so failed"));
    CHECK(line_with(text, "registered-natives:") == "registered-natives: 20");
}

void check_exit_reason() {
    zb::RuntimeReport report;
    report.note_guest_exit("guest SIGSEGV: read of 0x00000000, pc 0xf0001234 in liblime.so offset 0x1234");
    // The first reason wins: the crash says more than the exit status that follows it.
    report.note_guest_exit("guest exited with status 139");
    CHECK(line_with(report.text(), "guest-exit:") ==
          "guest-exit: guest SIGSEGV: read of 0x00000000, pc 0xf0001234 in liblime.so offset 0x1234");
}

void check_observer() {
    zb::RuntimeReport report;
    int structural = 0;
    int counter = 0;
    report.set_observer([&](bool is_structural) {
        if (is_structural) {
            ++structural;
        } else {
            ++counter;
        }
    });
    report.note_unimplemented_host_call(1, "libGLESv2.so", "glClear");  // new distinct: structural
    report.note_unimplemented_host_call(1, "libGLESv2.so", "glClear");  // repeat: counter only
    report.note_registered_native();                                    // counter only
    report.note_proxy_loaded("libstd.so", 0x00010006);                  // structural
    report.note_guest_exit("guest exited with status 0");               // structural
    report.note_guest_exit("ignored");                                  // no change, no callback
    CHECK(structural == 3);
    CHECK(counter == 2);

    report.set_observer(nullptr);
    report.note_registered_native();
    CHECK(structural == 3 && counter == 2);
}

void check_file_writer(const fs::path& dir) {
    const fs::path path = dir / "zb-runtime-report.txt";
    zb::RuntimeReport report;
    report.note_proxy_loaded("libstd.so", 0x00010006);
    // A long interval: only structural changes may rewrite the file during this test.
    CHECK(zb::write_runtime_report_to(report, path.string(), std::chrono::milliseconds(60000)));
    CHECK(fs::exists(path));
    CHECK(contains(read_file(path), "proxy-loaded: libstd.so jni=0x00010006"));
    // The temporary file is renamed, never left behind.
    CHECK(!fs::exists(fs::path(path.string() + ".tmp")));

    report.note_unimplemented_host_call(11, "libGLESv2.so", "glCreateProgram");
    CHECK(contains(read_file(path), "first-unimplemented: libGLESv2.so glCreateProgram"));

    // A counter-only change inside the interval is not written yet...
    report.note_unimplemented_host_call(11, "libGLESv2.so", "glCreateProgram");
    CHECK(contains(read_file(path), "unimplemented-host-calls: 1"));
    // ... but the next structural change publishes the counters with it.
    report.note_guest_exit("guest exited with status 0");
    const std::string final_text = read_file(path);
    CHECK(contains(final_text, "unimplemented-host-calls: 2"));
    CHECK(contains(final_text, "guest-exit: guest exited with status 0"));
    CHECK(final_text == report.text());

    // An unwritable path installs no observer and leaves the report usable.
    zb::RuntimeReport other;
    CHECK(!zb::write_runtime_report_to(other, (dir / "missing" / "report.txt").string()));
    other.note_registered_native();
    CHECK(other.registered_natives() == 1);
}

void check_throttle(const fs::path& dir) {
    const fs::path path = dir / "throttled.txt";
    zb::RuntimeReport report;
    CHECK(zb::write_runtime_report_to(report, path.string(), std::chrono::milliseconds(20)));
    for (int i = 0; i < 200; ++i) report.note_unimplemented_host_call(7, "libGLESv2.so", "glDrawArrays");
    // Hot counter-only traffic is throttled, so the file lags behind on purpose.
    CHECK(report.unimplemented_host_calls() == 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    report.note_unimplemented_host_call(7, "libGLESv2.so", "glDrawArrays");
    CHECK(contains(read_file(path), "unimplemented-host-calls: 201"));
}

void check_process_wide_instance() {
    // The process-wide report is the one Process and the JNI bridge write to.
    CHECK(&zb::runtime_report() == &zb::runtime_report());
    zb::runtime_report().clear();
    zb::runtime_report().note_registered_native();
    CHECK(zb::runtime_report().registered_natives() == 1);
    zb::runtime_report().clear();
    CHECK(zb::runtime_report().registered_natives() == 0);
}

void check_threads() {
    zb::RuntimeReport report;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < 500; ++i) {
                report.note_unimplemented_host_call(static_cast<std::uint32_t>(t), "libGLESv2.so", "glClear");
                report.note_registered_native();
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    CHECK(report.unimplemented_host_calls() == 2000);
    CHECK(report.registered_natives() == 2000);
    CHECK(count_with(report.text(), "unimplemented: ") == 4);
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("zb-runtime-report-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    check_empty_report();
    check_unimplemented_host_calls();
    check_host_call_bound();
    check_loads_and_natives();
    check_exit_reason();
    check_observer();
    check_file_writer(dir);
    check_throttle(dir);
    check_process_wide_instance();
    check_threads();
    check_gl_section();

    fs::remove_all(dir);
    std::puts("runtime_report_test PASS");
    return 0;
}
