#pragma once

#include <jni.h>

#include <mutex>
#include <string>

#include "zb/jni_backend.h"

namespace zb {

// JniBackend over the real JNIEnv / JavaVM of the app process (Android build only). Env values are
// host JNIEnv*, Refs are jobject, Ids are jmethodID / jfieldID. Each function is one JNI call (a few
// use reflection), so failures leave ART's pending exception exactly as native code would.
class JniEnvBackend final : public JniBackend {
public:
    explicit JniEnvBackend(JavaVM* vm);

    Ref find_class(Env env, const char* name) override;
    Ref get_superclass(Env env, Ref cls) override;
    bool is_assignable_from(Env env, Ref from, Ref to) override;
    Id get_method_id(Env env, Ref cls, const char* name, const char* signature, bool is_static) override;
    Id get_field_id(Env env, Ref cls, const char* name, const char* signature, bool is_static) override;
    Id from_reflected_method(Env env, Ref method, std::string& signature) override;
    Id from_reflected_field(Env env, Ref field) override;
    Ref to_reflected_method(Env env, Ref cls, Id method, bool is_static) override;
    Ref to_reflected_field(Env env, Ref cls, Id field, bool is_static) override;
    Ref alloc_object(Env env, Ref cls) override;
    Ref get_object_class(Env env, Ref obj) override;
    bool is_instance_of(Env env, Ref obj, Ref cls) override;
    bool is_same_object(Env env, Ref a, Ref b) override;
    JValue call_method(Env env, JniCallKind kind, char type, Ref obj, Ref cls, Id method,
                       const JValue* args) override;
    JValue get_field(Env env, bool is_static, char type, Ref obj, Id field) override;
    void set_field(Env env, bool is_static, char type, Ref obj, Id field, JValue value) override;
    Ref new_string(Env env, const std::uint16_t* chars, std::int32_t length) override;
    Ref new_string_utf(Env env, const char* utf) override;
    std::int32_t get_string_length(Env env, Ref str) override;
    std::int32_t get_string_utf_length(Env env, Ref str) override;
    void get_string_region(Env env, Ref str, std::int32_t start, std::int32_t length, void* out) override;
    bool get_string_utf_region(Env env, Ref str, std::int32_t start, std::int32_t length, std::string& out) override;
    std::int32_t get_array_length(Env env, Ref array) override;
    char get_array_element_type(Env env, Ref array) override;
    Ref new_object_array(Env env, std::int32_t length, Ref element_class, Ref initial) override;
    Ref get_object_array_element(Env env, Ref array, std::int32_t index) override;
    void set_object_array_element(Env env, Ref array, std::int32_t index, Ref value) override;
    Ref new_primitive_array(Env env, char type, std::int32_t length) override;
    void get_primitive_array_region(Env env, char type, Ref array, std::int32_t start, std::int32_t length,
                                    void* out) override;
    void set_primitive_array_region(Env env, char type, Ref array, std::int32_t start, std::int32_t length,
                                    const void* in) override;
    Ref new_global_ref(Env env, Ref obj) override;
    void delete_global_ref(Env env, Ref ref) override;
    Ref new_weak_global_ref(Env env, Ref obj) override;
    void delete_weak_global_ref(Env env, Ref ref) override;
    Ref new_local_ref(Env env, Ref obj) override;
    void delete_local_ref(Env env, Ref ref) override;
    std::int32_t ensure_local_capacity(Env env, std::int32_t capacity) override;
    std::int32_t push_local_frame(Env env, std::int32_t capacity) override;
    Ref pop_local_frame(Env env, Ref result) override;
    std::int32_t throw_exception(Env env, Ref throwable) override;
    std::int32_t throw_new(Env env, Ref cls, const char* message) override;
    Ref exception_occurred(Env env) override;
    void exception_describe(Env env) override;
    void exception_clear(Env env) override;
    bool exception_check(Env env) override;
    void fatal_error(Env env, const char* message) override;
    std::int32_t monitor_enter(Env env, Ref obj) override;
    std::int32_t monitor_exit(Env env, Ref obj) override;
    std::int32_t register_native(Env env, Ref cls, const char* name, const char* signature, void* function) override;
    std::int32_t unregister_natives(Env env, Ref cls) override;
    Ref new_direct_byte_buffer(Env env, void* address, std::int64_t capacity) override;
    void* get_direct_buffer_address(Env env, Ref buffer) override;
    std::int64_t get_direct_buffer_capacity(Env env, Ref buffer) override;
    Env attach_current_thread(bool daemon, const char* name, Ref group) override;
    std::int32_t detach_current_thread() override;

private:
    // Reflection ids, looked up once (method ids of boot classes stay valid for the process).
    struct Reflection {
        jmethodID class_get_name = nullptr;          // Class.getName()
        jmethodID class_is_primitive = nullptr;      // Class.isPrimitive()
        jmethodID method_get_return_type = nullptr;  // Method.getReturnType()
        jmethodID executable_get_parameter_types = nullptr;
        jclass constructor_class = nullptr;          // global reference
    };
    const Reflection* reflection(JNIEnv* env);
    // Descriptor letter of a java.lang.Class ('L' for references and arrays); 0 on failure.
    char type_letter(JNIEnv* env, jobject type);

    JavaVM* vm_;
    std::once_flag reflection_once_;
    Reflection reflection_;
    bool reflection_ok_ = false;
};

}  // namespace zb
