// The JNI bridge end to end on this machine: libzbjni.so inside the library runtime, the flat JNI
// host calls against the mock JVM, and the guest probe library guest/testlib/zbjniprobe.c.
// Usage: jni_bridge_test <sysroot> <zbhost> <guest lib dir>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "check.h"
#include "mock_jvm.h"
#include "zb/host_jni.h"
#include "zb/library_runtime.h"

namespace {

using namespace std::chrono_literals;
using zb::JValue;
using zb::mock::MockCall;
using zb::mock::MockJvm;

JValue value_i(std::int32_t v) {
    JValue value{};
    value.i = v;
    return value;
}

std::u16string to_u16(const std::string& ascii) {
    return std::u16string(ascii.begin(), ascii.end());
}

// zb/Probe and zb/ProbeChild: the Java side of the guest-called JNI probes.
void define_probe_model(MockJvm& vm) {
    vm.define_class("zb/Probe");
    vm.define_class("zb/ProbeChild", "zb/Probe");
    vm.define_class("java/lang/IllegalStateException", "java/lang/RuntimeException");
    struct Type {
        char letter;
        const char* descriptor;
        const char* field;
    };
    const Type types[] = {{'Z', "Z", "z"}, {'B', "B", "b"}, {'C', "C", "c"}, {'S', "S", "s"}, {'I', "I", "i"},
                          {'J', "J", "j"}, {'F', "F", "f"}, {'D', "D", "d"}, {'L', "Ljava/lang/String;", "l"}};
    for (const Type& type : types) {
        const std::string signature = std::string("(") + type.descriptor + ")" + type.descriptor;
        const std::string echo = std::string("echo") + type.letter;
        const char letter = type.letter;
        // step 1: zb/Probe (arg+1, !z, *2, same reference); step 2: zb/ProbeChild (arg+2, z, *4, null).
        const auto body = [letter](int step) {
            return [letter, step](MockCall& call) {
                JValue out = call.args[0];
                switch (letter) {
                case 'Z': out.z = step == 1 ? !out.z : out.z; break;
                case 'B': out.b = static_cast<std::int8_t>(out.b + step); break;
                case 'C': out.c = static_cast<std::uint16_t>(out.c + step); break;
                case 'S': out.s = static_cast<std::int16_t>(out.s + step); break;
                case 'I': out.i += step; break;
                case 'J': out.j += step; break;
                case 'F': out.f *= static_cast<float>(2 * step); break;
                case 'D': out.d *= 2.0 * step; break;
                default: out.l = step == 1 ? out.l : 0; break;
                }
                return out;
            };
        };
        vm.add_method("zb/Probe", echo, signature, false, body(1));
        vm.add_method("zb/ProbeChild", echo, signature, false, body(2));
        vm.add_method("zb/Probe", "s" + echo, signature, true, body(1));
        vm.add_field("zb/Probe", type.field, type.descriptor, false);
        vm.add_field("zb/Probe", std::string("s") + type.field, type.descriptor, true);
    }
    const auto bump = [](std::int32_t step) {
        return [step](MockCall& call) {
            const auto cls = call.vm.class_object("zb/Probe");
            call.vm.set_field_value(cls, "calls", value_i(call.vm.field_value(cls, "calls").i + step));
            return JValue{};
        };
    };
    vm.add_method("zb/Probe", "echoV", "()V", false, bump(1));
    vm.add_method("zb/ProbeChild", "echoV", "()V", false, bump(2));
    vm.add_method("zb/Probe", "sechoV", "()V", true, bump(4));
    vm.add_field("zb/Probe", "calls", "I", true);
    vm.add_method("zb/Probe", "mix", "(ZBCSIJFDLjava/lang/String;)I", true, [](MockCall& call) {
        const auto& a = call.args;
        const bool ok = a[0].z == 1 && a[1].b == -2 && a[2].c == 0x1234 && a[3].s == -3 && a[4].i == 4 &&
                        a[5].j == 0x1122334455667788LL && a[6].f == 1.5f && a[7].d == -2.25 &&
                        call.vm.string_value(a[8].l) == u"text";
        return value_i(ok ? 42 : 0);
    });
    vm.add_method("zb/Probe", "<init>", "(ILjava/lang/String;)V", false, [](MockCall& call) {
        call.vm.set_field_value(call.self, "i", call.args[0]);
        call.vm.set_field_value(call.self, "l", call.args[1]);
        return JValue{};
    });
    vm.add_method("zb/Probe", "fail", "()V", true, [](MockCall& call) {
        call.vm.throw_in(call.env, "java/lang/IllegalStateException", "boom");
        return JValue{};
    });
}

zb::LibraryRuntimeOptions options(char** argv) {
    zb::LibraryRuntimeOptions result;
    result.sysroot = argv[1];
    result.zbhost = argv[2];
    result.target_sdk = 16;
    result.guest_environment = {std::string("LD_LIBRARY_PATH=") + argv[3]};
    result.preload = "libzbjni.so";
    return result;
}

struct Bridge {
    MockJvm* vm;
    zb::LibraryRuntime* runtime;
    zb::HostJni* jni;
    std::uint32_t library;
};

// Process-lifetime objects: never destroyed (tests end with _Exit).
Bridge start_bridge(char** argv) {
    Bridge bridge{};
    bridge.vm = new MockJvm();
    define_probe_model(*bridge.vm);
    bridge.runtime = new zb::LibraryRuntime();
    bridge.jni = new zb::HostJni(*bridge.runtime, *bridge.vm);
    zb::HostJni* jni = bridge.jni;
    bridge.runtime->set_host_call_handler(
        [jni](std::uint32_t index, zb::GuestThread& thread) { return jni->handle_host_call(index, thread); });
    std::string error;
    CHECK(bridge.runtime->start(options(argv), error));
    CHECK(bridge.jni->ready() && bridge.jni->guest_java_vm() != 0);
    bridge.library = bridge.runtime->load_library(std::string(argv[3]) + "/libzbjniprobe.so", ZB_GUEST_RTLD_NOW, error);
    CHECK(bridge.library != 0);
    return bridge;
}

std::uint32_t symbol(Bridge& bridge, const char* name) {
    std::string error;
    const std::uint32_t address = bridge.runtime->find_symbol(bridge.library, name, error);
    if (address == 0) std::fprintf(stderr, "missing guest symbol %s: %s\n", name, error.c_str());
    CHECK(address != 0);
    return address;
}

// Calls a probe as a native method of the calling Java thread with one object argument (or null).
// Returns 0, or the probe's failing source line.
std::int32_t run_probe(Bridge& bridge, const char* name, MockJvm::ObjectId argument = 0) {
    const std::uint32_t function = symbol(bridge, name);
    const auto env = bridge.vm->thread_env();
    MockJvm::NativeFrame frame(*bridge.vm, env);
    const auto ref = argument != 0 ? frame.local(argument) : 0;
    const auto result =
        bridge.jni->call_native(env, 'I', function, [&](std::uint32_t guest_env, const zb::RefToHandle& to_handle) {
            zb::GuestCall call;
            call.regs = {guest_env, to_handle(ref), 0, 0};
            return call;
        });
    CHECK(result);
    const auto line = static_cast<std::int32_t>(result->guest.r0);
    if (line != 0) std::fprintf(stderr, "%s failed at zbjniprobe.c:%d\n", name, line);
    // The call's frame released everything the guest created; only the argument is left.
    CHECK(frame.close() == (argument != 0 ? 1u : 0u));
    return line;
}

// An invalid handle ends the process through the backend's FatalError.
void check_invalid_handle(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        Bridge bridge = start_bridge(argv);
        run_probe(bridge, "zbjniprobe_bad_handle");
        std::_Exit(10);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == zb::mock::kFatalExitStatus);
}

void check_objects(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    const auto held = vm.new_object("zb/Probe");
    CHECK(run_probe(bridge, "zbjniprobe_objects", held) == 0);
    CHECK(vm.live_global_refs() == 1 && vm.live_weak_refs() == 1);
    vm.collect(held);
    CHECK(run_probe(bridge, "zbjniprobe_objects_after_gc") == 0);
    CHECK(vm.live_global_refs() == 0 && vm.live_weak_refs() == 0);

    // An exception thrown by the guest stays pending for the Java caller.
    const auto env = vm.thread_env();
    CHECK(run_probe(bridge, "zbjniprobe_throw") == 0);
    const auto pending = vm.pending_exception(env);
    CHECK(pending != 0 && vm.class_name_of(pending) == "java/lang/IllegalArgumentException");
    CHECK(vm.string_value(pending) == u"from guest");
    vm.clear_pending_exception(env);
}

void check_values(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    CHECK(run_probe(bridge, "zbjniprobe_calls", vm.new_string_object(u"text")) == 0);
    CHECK(run_probe(bridge, "zbjniprobe_fields", vm.new_string_object(u"field")) == 0);
    const auto probe = vm.class_object("zb/Probe");
    CHECK(vm.field_value(probe, "sz").z == 1 && vm.field_value(probe, "sc").c == 0xBEEF);
    CHECK(vm.field_value(probe, "sj").j == -0x0123456789abcdefLL && vm.field_value(probe, "sf").f == -0.375f);
    CHECK(vm.field_value(probe, "sd").d == 6.02214076e23 && vm.string_value(vm.field_value(probe, "sl").l) == u"field");

    const auto env = vm.thread_env();
    CHECK(run_probe(bridge, "zbjniprobe_exceptions") == 0);
    const auto pending = vm.pending_exception(env);
    CHECK(pending != 0 && vm.class_name_of(pending) == "java/lang/IllegalStateException");
    CHECK(vm.string_value(pending) == u"boom");
    vm.clear_pending_exception(env);
}

void check_data(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    CHECK(run_probe(bridge, "zbjniprobe_strings") == 0);
    CHECK(run_probe(bridge, "zbjniprobe_arrays") == 0);
    static char foreign[16];
    CHECK(run_probe(bridge, "zbjniprobe_direct_buffers", vm.new_direct_buffer_object(foreign, sizeof foreign)) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    check_invalid_handle(argv);
    Bridge bridge = start_bridge(argv);
    check_objects(bridge);
    check_values(bridge);
    check_data(bridge);
    const auto errors = bridge.vm->errors();
    for (const auto& error : errors) std::fprintf(stderr, "mock error: %s\n", error.c_str());
    CHECK(errors.empty());
    std::puts("jni_bridge_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
