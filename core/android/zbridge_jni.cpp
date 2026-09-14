// JNI entry points of libzbridge.so for com.zettabridge.core.ZBridge.
#include <jni.h>

#include <string>
#include <vector>

#include "zb/zbridge.h"

namespace {

std::vector<std::string> to_strings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> out;
    if (array == nullptr) return out;
    const jsize count = env->GetArrayLength(array);
    out.reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        if (value == nullptr) {
            out.emplace_back();
            continue;
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        out.emplace_back(chars != nullptr ? chars : "");
        if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
    }
    return out;
}

std::vector<const char*> to_pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (const auto& s : strings) pointers.push_back(s.c_str());
    pointers.push_back(nullptr);
    return pointers;
}

}  // namespace

extern "C" {

// static native int runExecutable(String sysroot, String[] argv, String[] envp)
// Blocks until the guest exits. envp == null passes the app process environment.
JNIEXPORT jint JNICALL Java_com_zettabridge_core_ZBridge_runExecutable(JNIEnv* env, jclass, jstring sysroot,
                                                                      jobjectArray argv, jobjectArray envp) {
    std::string sysroot_path;
    if (sysroot != nullptr) {
        const char* chars = env->GetStringUTFChars(sysroot, nullptr);
        if (chars != nullptr) {
            sysroot_path = chars;
            env->ReleaseStringUTFChars(sysroot, chars);
        }
    }

    const std::vector<std::string> args = to_strings(env, argv);
    if (args.empty()) return 2;
    const std::vector<const char*> arg_pointers = to_pointers(args);

    std::vector<std::string> env_strings;
    std::vector<const char*> env_pointers;
    if (envp != nullptr) {
        env_strings = to_strings(env, envp);
        env_pointers = to_pointers(env_strings);
    }

    return zb_run_executable(sysroot != nullptr ? sysroot_path.c_str() : nullptr, static_cast<int>(args.size()),
                             arg_pointers.data(), envp != nullptr ? env_pointers.data() : nullptr);
}

}  // extern "C"
