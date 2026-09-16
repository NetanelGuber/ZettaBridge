// Executable fake-JNI checks for the real ART backend's class-loader routing.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "jni_env_backend.h"

namespace {

#define CHECK(condition)                                                                            \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);      \
            std::fflush(stderr);                                                                    \
            std::_Exit(1);                                                                          \
        }                                                                                           \
    } while (0)

struct Fake {
    int find_class_calls = 0;
    int loader_calls = 0;
    char last_find[128] = {};
    char load_name[128] = {};
};

Fake fake;
int class_token;
int loader_token;
int plugin_class_token;
int string_token;
int method_token;
int load_class_method_token;

jclass JNICALL find_class(JNIEnv*, const char* name) {
    ++fake.find_class_calls;
    std::snprintf(fake.last_find, sizeof fake.last_find, "%s", name);
    return reinterpret_cast<jclass>(&class_token);
}

jobject JNICALL new_global_ref(JNIEnv*, jobject object) {
    return object;
}

void JNICALL delete_local_ref(JNIEnv*, jobject) {}

jmethodID JNICALL get_method_id(JNIEnv*, jclass, const char* name, const char*) {
    return reinterpret_cast<jmethodID>(std::strcmp(name, "loadClass") == 0 ? &load_class_method_token
                                                                          : &method_token);
}

jmethodID JNICALL get_static_method_id(JNIEnv*, jclass, const char*, const char*) {
    return reinterpret_cast<jmethodID>(&method_token);
}

jboolean JNICALL is_instance_of(JNIEnv*, jobject, jclass) {
    return JNI_TRUE;
}

jboolean JNICALL exception_check(JNIEnv*) {
    return JNI_FALSE;
}

void JNICALL exception_clear(JNIEnv*) {}

jstring JNICALL new_string_utf(JNIEnv*, const char* value) {
    std::snprintf(fake.load_name, sizeof fake.load_name, "%s", value);
    return reinterpret_cast<jstring>(&string_token);
}

jobject JNICALL call_object_method_v(JNIEnv*, jobject object, jmethodID method, va_list args) {
    CHECK(object == reinterpret_cast<jobject>(&loader_token));
    CHECK(method == reinterpret_cast<jmethodID>(&load_class_method_token));
    CHECK(va_arg(args, jstring) == reinterpret_cast<jstring>(&string_token));
    ++fake.loader_calls;
    return reinterpret_cast<jobject>(&plugin_class_token);
}

}  // namespace

int main() {
    JNINativeInterface_ functions{};
    functions.FindClass = find_class;
    functions.NewGlobalRef = new_global_ref;
    functions.DeleteLocalRef = delete_local_ref;
    functions.GetMethodID = get_method_id;
    functions.GetStaticMethodID = get_static_method_id;
    functions.IsInstanceOf = is_instance_of;
    functions.ExceptionCheck = exception_check;
    functions.ExceptionClear = exception_clear;
    functions.NewStringUTF = new_string_utf;
    functions.CallObjectMethodV = call_object_method_v;
    JNIEnv env{&functions};

    zb::JniEnvBackend backend(nullptr);
    CHECK(backend.set_class_loader(&env, reinterpret_cast<jobject>(&loader_token)));
    const int setup_find_calls = fake.find_class_calls;
    const auto found = backend.find_class(reinterpret_cast<std::uintptr_t>(&env), "zb/Natives");

    CHECK(found == reinterpret_cast<std::uintptr_t>(&plugin_class_token));
    CHECK(fake.find_class_calls == setup_find_calls);
    CHECK(fake.loader_calls == 1);
    CHECK(std::strcmp(fake.load_name, "zb.Natives") == 0);
    std::puts("jni env backend: plugin class-loader routing passed");
    return 0;
}
