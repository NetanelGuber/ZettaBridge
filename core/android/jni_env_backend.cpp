// JniBackend over the real JNIEnv (Android build only; compile-checked here, exercised by T7 in 4d).
#include "jni_env_backend.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace zb {

namespace {

static_assert(sizeof(jvalue) == sizeof(JValue) && alignof(jvalue) == alignof(JValue));

JNIEnv* E(JniBackend::Env env) {
    return reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(env));
}

jobject O(JniBackend::Ref ref) {
    return reinterpret_cast<jobject>(static_cast<std::uintptr_t>(ref));
}

jclass C(JniBackend::Ref ref) {
    return static_cast<jclass>(O(ref));
}

JniBackend::Ref R(jobject obj) {
    return static_cast<JniBackend::Ref>(reinterpret_cast<std::uintptr_t>(obj));
}

jmethodID M(JniBackend::Id id) {
    return reinterpret_cast<jmethodID>(static_cast<std::uintptr_t>(id));
}

jfieldID F(JniBackend::Id id) {
    return reinterpret_cast<jfieldID>(static_cast<std::uintptr_t>(id));
}

JniBackend::Id I(void* id) {
    return static_cast<JniBackend::Id>(reinterpret_cast<std::uintptr_t>(id));
}

}  // namespace

JniEnvBackend::JniEnvBackend(JavaVM* vm) : vm_(vm) {}

const JniEnvBackend::Reflection* JniEnvBackend::reflection(JNIEnv* env) {
    std::call_once(reflection_once_, [&] {
        jclass class_class = env->FindClass("java/lang/Class");
        jclass method_class = env->FindClass("java/lang/reflect/Method");
        jclass executable_class = env->FindClass("java/lang/reflect/Executable");
        jclass constructor_class = env->FindClass("java/lang/reflect/Constructor");
        if (class_class == nullptr || method_class == nullptr || executable_class == nullptr ||
            constructor_class == nullptr) {
            env->ExceptionClear();
            return;
        }
        reflection_.class_get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
        reflection_.class_is_primitive = env->GetMethodID(class_class, "isPrimitive", "()Z");
        reflection_.method_get_return_type = env->GetMethodID(method_class, "getReturnType", "()Ljava/lang/Class;");
        reflection_.executable_get_parameter_types =
            env->GetMethodID(executable_class, "getParameterTypes", "()[Ljava/lang/Class;");
        reflection_.constructor_class = static_cast<jclass>(env->NewGlobalRef(constructor_class));
        reflection_ok_ = reflection_.class_get_name != nullptr && reflection_.class_is_primitive != nullptr &&
                         reflection_.method_get_return_type != nullptr &&
                         reflection_.executable_get_parameter_types != nullptr;
        if (!reflection_ok_) env->ExceptionClear();
        env->DeleteLocalRef(class_class);
        env->DeleteLocalRef(method_class);
        env->DeleteLocalRef(executable_class);
        env->DeleteLocalRef(constructor_class);
    });
    return reflection_ok_ ? &reflection_ : nullptr;
}

char JniEnvBackend::type_letter(JNIEnv* env, jobject type) {
    const Reflection* r = reflection(env);
    if (r == nullptr || type == nullptr) return 0;
    if (!env->CallBooleanMethod(type, r->class_is_primitive)) return env->ExceptionCheck() ? 0 : 'L';
    auto name = static_cast<jstring>(env->CallObjectMethod(type, r->class_get_name));
    if (name == nullptr) return 0;
    const char* chars = env->GetStringUTFChars(name, nullptr);
    const std::string text = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(name, chars);
    env->DeleteLocalRef(name);
    static const struct {
        const char* name;
        char letter;
    } kPrimitives[] = {{"boolean", 'Z'}, {"byte", 'B'}, {"char", 'C'}, {"short", 'S'}, {"int", 'I'},
                       {"long", 'J'},    {"float", 'F'}, {"double", 'D'}, {"void", 'V'}};
    for (const auto& primitive : kPrimitives) {
        if (text == primitive.name) return primitive.letter;
    }
    return 0;
}

JniBackend::Ref JniEnvBackend::find_class(Env env, const char* name) {
    return R(E(env)->FindClass(name));
}

JniBackend::Ref JniEnvBackend::get_superclass(Env env, Ref cls) {
    return R(E(env)->GetSuperclass(C(cls)));
}

bool JniEnvBackend::is_assignable_from(Env env, Ref from, Ref to) {
    return E(env)->IsAssignableFrom(C(from), C(to)) == JNI_TRUE;
}

JniBackend::Id JniEnvBackend::get_method_id(Env env, Ref cls, const char* name, const char* signature,
                                            bool is_static) {
    return I(is_static ? E(env)->GetStaticMethodID(C(cls), name, signature)
                       : E(env)->GetMethodID(C(cls), name, signature));
}

JniBackend::Id JniEnvBackend::get_field_id(Env env, Ref cls, const char* name, const char* signature,
                                           bool is_static) {
    return I(is_static ? E(env)->GetStaticFieldID(C(cls), name, signature)
                       : E(env)->GetFieldID(C(cls), name, signature));
}

// The descriptor is rebuilt from reflection with every reference type erased to Object, which
// keeps the shorty exact.
JniBackend::Id JniEnvBackend::from_reflected_method(Env env, Ref method, std::string& signature) {
    JNIEnv* e = E(env);
    const Reflection* r = reflection(e);
    const jmethodID id = e->FromReflectedMethod(O(method));
    if (id == nullptr || r == nullptr) return 0;
    const auto letter_descriptor = [](char letter) {
        return letter == 'L' ? std::string("Ljava/lang/Object;") : std::string(1, letter);
    };
    std::string descriptor = "(";
    auto parameters = static_cast<jobjectArray>(e->CallObjectMethod(O(method), r->executable_get_parameter_types));
    if (parameters == nullptr) return 0;
    const jsize count = e->GetArrayLength(parameters);
    for (jsize i = 0; i < count; ++i) {
        jobject type = e->GetObjectArrayElement(parameters, i);
        const char letter = type_letter(e, type);
        e->DeleteLocalRef(type);
        if (letter == 0 || letter == 'V') {
            e->DeleteLocalRef(parameters);
            return 0;
        }
        descriptor += letter_descriptor(letter);
    }
    e->DeleteLocalRef(parameters);
    char result = 'V';
    if (!e->IsInstanceOf(O(method), r->constructor_class)) {
        jobject type = e->CallObjectMethod(O(method), r->method_get_return_type);
        result = type_letter(e, type);
        e->DeleteLocalRef(type);
        if (result == 0) return 0;
    }
    signature = descriptor + ")" + (result == 'V' ? std::string("V") : letter_descriptor(result));
    return I(id);
}

JniBackend::Id JniEnvBackend::from_reflected_field(Env env, Ref field) {
    return I(E(env)->FromReflectedField(O(field)));
}

JniBackend::Ref JniEnvBackend::to_reflected_method(Env env, Ref cls, Id method, bool is_static) {
    return R(E(env)->ToReflectedMethod(C(cls), M(method), is_static ? JNI_TRUE : JNI_FALSE));
}

JniBackend::Ref JniEnvBackend::to_reflected_field(Env env, Ref cls, Id field, bool is_static) {
    return R(E(env)->ToReflectedField(C(cls), F(field), is_static ? JNI_TRUE : JNI_FALSE));
}

JniBackend::Ref JniEnvBackend::alloc_object(Env env, Ref cls) {
    return R(E(env)->AllocObject(C(cls)));
}

JniBackend::Ref JniEnvBackend::get_object_class(Env env, Ref obj) {
    return R(E(env)->GetObjectClass(O(obj)));
}

bool JniEnvBackend::is_instance_of(Env env, Ref obj, Ref cls) {
    return E(env)->IsInstanceOf(O(obj), C(cls)) == JNI_TRUE;
}

bool JniEnvBackend::is_same_object(Env env, Ref a, Ref b) {
    return E(env)->IsSameObject(O(a), O(b)) == JNI_TRUE;
}

JValue JniEnvBackend::call_method(Env env, JniCallKind kind, char type, Ref obj, Ref cls, Id method,
                                  const JValue* args) {
    JNIEnv* e = E(env);
    const auto* a = reinterpret_cast<const jvalue*>(args);
    const jmethodID m = M(method);
    JValue out{};
#define ZB_CALL(Name, member, wrap)                                                       \
    switch (kind) {                                                                       \
    case JniCallKind::Virtual: member wrap(e->Call##Name##MethodA(O(obj), m, a)); break;  \
    case JniCallKind::Nonvirtual:                                                         \
        member wrap(e->CallNonvirtual##Name##MethodA(O(obj), C(cls), m, a));              \
        break;                                                                            \
    case JniCallKind::Static: member wrap(e->CallStatic##Name##MethodA(C(cls), m, a)); break; \
    case JniCallKind::NewObject: break;                                                   \
    }
    if (kind == JniCallKind::NewObject) {
        out.l = R(e->NewObjectA(C(cls), m, a));
        return out;
    }
    switch (type) {
    case 'Z': ZB_CALL(Boolean, out.z =, ) break;
    case 'B': ZB_CALL(Byte, out.b =, ) break;
    case 'C': ZB_CALL(Char, out.c =, ) break;
    case 'S': ZB_CALL(Short, out.s =, ) break;
    case 'I': ZB_CALL(Int, out.i =, ) break;
    case 'J': ZB_CALL(Long, out.j =, ) break;
    case 'F': ZB_CALL(Float, out.f =, ) break;
    case 'D': ZB_CALL(Double, out.d =, ) break;
    case 'L': ZB_CALL(Object, out.l =, R) break;
    default: ZB_CALL(Void, , ) break;
    }
#undef ZB_CALL
    return out;
}

JValue JniEnvBackend::get_field(Env env, bool is_static, char type, Ref obj, Id field) {
    JNIEnv* e = E(env);
    const jfieldID f = F(field);
    JValue out{};
#define ZB_GET(Name, member, wrap)                                                      \
    member wrap(is_static ? e->GetStatic##Name##Field(C(obj), f) : e->Get##Name##Field(O(obj), f))
    switch (type) {
    case 'Z': ZB_GET(Boolean, out.z =, ); break;
    case 'B': ZB_GET(Byte, out.b =, ); break;
    case 'C': ZB_GET(Char, out.c =, ); break;
    case 'S': ZB_GET(Short, out.s =, ); break;
    case 'I': ZB_GET(Int, out.i =, ); break;
    case 'J': ZB_GET(Long, out.j =, ); break;
    case 'F': ZB_GET(Float, out.f =, ); break;
    case 'D': ZB_GET(Double, out.d =, ); break;
    default: ZB_GET(Object, out.l =, R); break;
    }
#undef ZB_GET
    return out;
}

void JniEnvBackend::set_field(Env env, bool is_static, char type, Ref obj, Id field, JValue value) {
    JNIEnv* e = E(env);
    const jfieldID f = F(field);
#define ZB_SET(Name, v)                                                                 \
    if (is_static) {                                                                    \
        e->SetStatic##Name##Field(C(obj), f, v);                                        \
    } else {                                                                            \
        e->Set##Name##Field(O(obj), f, v);                                              \
    }
    switch (type) {
    case 'Z': ZB_SET(Boolean, value.z) break;
    case 'B': ZB_SET(Byte, value.b) break;
    case 'C': ZB_SET(Char, value.c) break;
    case 'S': ZB_SET(Short, value.s) break;
    case 'I': ZB_SET(Int, value.i) break;
    case 'J': ZB_SET(Long, value.j) break;
    case 'F': ZB_SET(Float, value.f) break;
    case 'D': ZB_SET(Double, value.d) break;
    default: ZB_SET(Object, O(value.l)) break;
    }
#undef ZB_SET
}

JniBackend::Ref JniEnvBackend::new_string(Env env, const std::uint16_t* chars, std::int32_t length) {
    // Guest buffers may be unaligned for jchar.
    if (length > 0 && (reinterpret_cast<std::uintptr_t>(chars) & 1u) != 0) {
        std::vector<jchar> copy(static_cast<std::size_t>(length));
        std::memcpy(copy.data(), chars, copy.size() * sizeof(jchar));
        return R(E(env)->NewString(copy.data(), length));
    }
    return R(E(env)->NewString(chars, length));
}

JniBackend::Ref JniEnvBackend::new_string_utf(Env env, const char* utf) {
    return R(E(env)->NewStringUTF(utf));
}

std::int32_t JniEnvBackend::get_string_length(Env env, Ref str) {
    return E(env)->GetStringLength(static_cast<jstring>(O(str)));
}

std::int32_t JniEnvBackend::get_string_utf_length(Env env, Ref str) {
    return E(env)->GetStringUTFLength(static_cast<jstring>(O(str)));
}

void JniEnvBackend::get_string_region(Env env, Ref str, std::int32_t start, std::int32_t length, void* out) {
    if (length > 0 && (reinterpret_cast<std::uintptr_t>(out) & 1u) != 0) {
        std::vector<jchar> copy(static_cast<std::size_t>(length));
        E(env)->GetStringRegion(static_cast<jstring>(O(str)), start, length, copy.data());
        if (!E(env)->ExceptionCheck()) std::memcpy(out, copy.data(), copy.size() * sizeof(jchar));
        return;
    }
    E(env)->GetStringRegion(static_cast<jstring>(O(str)), start, length, static_cast<jchar*>(out));
}

bool JniEnvBackend::get_string_utf_region(Env env, Ref str, std::int32_t start, std::int32_t length,
                                          std::string& out) {
    JNIEnv* e = E(env);
    // Modified UTF-8 needs at most 3 bytes per UTF-16 unit and never contains a zero byte.
    std::vector<char> buffer(length > 0 ? 3 * static_cast<std::size_t>(length) + 1 : 1, '\0');
    e->GetStringUTFRegion(static_cast<jstring>(O(str)), start, length, buffer.data());
    if (e->ExceptionCheck()) return false;
    out.assign(buffer.data());
    return true;
}

std::int32_t JniEnvBackend::get_array_length(Env env, Ref array) {
    return E(env)->GetArrayLength(static_cast<jarray>(O(array)));
}

char JniEnvBackend::get_array_element_type(Env env, Ref array) {
    JNIEnv* e = E(env);
    const Reflection* r = reflection(e);
    if (r == nullptr || array == 0) return 0;
    jclass cls = e->GetObjectClass(O(array));
    auto name = static_cast<jstring>(e->CallObjectMethod(cls, r->class_get_name));
    e->DeleteLocalRef(cls);
    if (name == nullptr) return 0;
    const char* chars = e->GetStringUTFChars(name, nullptr);
    char letter = 0;
    if (chars != nullptr && chars[0] == '[') letter = chars[1] == '[' || chars[1] == 'L' ? 'L' : chars[1];
    if (chars != nullptr) e->ReleaseStringUTFChars(name, chars);
    e->DeleteLocalRef(name);
    return letter;
}

JniBackend::Ref JniEnvBackend::new_object_array(Env env, std::int32_t length, Ref element_class, Ref initial) {
    return R(E(env)->NewObjectArray(length, C(element_class), O(initial)));
}

JniBackend::Ref JniEnvBackend::get_object_array_element(Env env, Ref array, std::int32_t index) {
    return R(E(env)->GetObjectArrayElement(static_cast<jobjectArray>(O(array)), index));
}

void JniEnvBackend::set_object_array_element(Env env, Ref array, std::int32_t index, Ref value) {
    E(env)->SetObjectArrayElement(static_cast<jobjectArray>(O(array)), index, O(value));
}

JniBackend::Ref JniEnvBackend::new_primitive_array(Env env, char type, std::int32_t length) {
    JNIEnv* e = E(env);
    switch (type) {
    case 'Z': return R(e->NewBooleanArray(length));
    case 'B': return R(e->NewByteArray(length));
    case 'C': return R(e->NewCharArray(length));
    case 'S': return R(e->NewShortArray(length));
    case 'I': return R(e->NewIntArray(length));
    case 'J': return R(e->NewLongArray(length));
    case 'F': return R(e->NewFloatArray(length));
    case 'D': return R(e->NewDoubleArray(length));
    default: return 0;
    }
}

// ART copies array regions with memcpy, so unaligned guest buffers are fine.
void JniEnvBackend::get_primitive_array_region(Env env, char type, Ref array, std::int32_t start,
                                               std::int32_t length, void* out) {
    JNIEnv* e = E(env);
    switch (type) {
    case 'Z': e->GetBooleanArrayRegion(static_cast<jbooleanArray>(O(array)), start, length, static_cast<jboolean*>(out)); break;
    case 'B': e->GetByteArrayRegion(static_cast<jbyteArray>(O(array)), start, length, static_cast<jbyte*>(out)); break;
    case 'C': e->GetCharArrayRegion(static_cast<jcharArray>(O(array)), start, length, static_cast<jchar*>(out)); break;
    case 'S': e->GetShortArrayRegion(static_cast<jshortArray>(O(array)), start, length, static_cast<jshort*>(out)); break;
    case 'I': e->GetIntArrayRegion(static_cast<jintArray>(O(array)), start, length, static_cast<jint*>(out)); break;
    case 'J': e->GetLongArrayRegion(static_cast<jlongArray>(O(array)), start, length, static_cast<jlong*>(out)); break;
    case 'F': e->GetFloatArrayRegion(static_cast<jfloatArray>(O(array)), start, length, static_cast<jfloat*>(out)); break;
    case 'D': e->GetDoubleArrayRegion(static_cast<jdoubleArray>(O(array)), start, length, static_cast<jdouble*>(out)); break;
    default: break;
    }
}

void JniEnvBackend::set_primitive_array_region(Env env, char type, Ref array, std::int32_t start,
                                               std::int32_t length, const void* in) {
    JNIEnv* e = E(env);
    switch (type) {
    case 'Z': e->SetBooleanArrayRegion(static_cast<jbooleanArray>(O(array)), start, length, static_cast<const jboolean*>(in)); break;
    case 'B': e->SetByteArrayRegion(static_cast<jbyteArray>(O(array)), start, length, static_cast<const jbyte*>(in)); break;
    case 'C': e->SetCharArrayRegion(static_cast<jcharArray>(O(array)), start, length, static_cast<const jchar*>(in)); break;
    case 'S': e->SetShortArrayRegion(static_cast<jshortArray>(O(array)), start, length, static_cast<const jshort*>(in)); break;
    case 'I': e->SetIntArrayRegion(static_cast<jintArray>(O(array)), start, length, static_cast<const jint*>(in)); break;
    case 'J': e->SetLongArrayRegion(static_cast<jlongArray>(O(array)), start, length, static_cast<const jlong*>(in)); break;
    case 'F': e->SetFloatArrayRegion(static_cast<jfloatArray>(O(array)), start, length, static_cast<const jfloat*>(in)); break;
    case 'D': e->SetDoubleArrayRegion(static_cast<jdoubleArray>(O(array)), start, length, static_cast<const jdouble*>(in)); break;
    default: break;
    }
}

JniBackend::Ref JniEnvBackend::new_global_ref(Env env, Ref obj) {
    return R(E(env)->NewGlobalRef(O(obj)));
}

void JniEnvBackend::delete_global_ref(Env env, Ref ref) {
    E(env)->DeleteGlobalRef(O(ref));
}

JniBackend::Ref JniEnvBackend::new_weak_global_ref(Env env, Ref obj) {
    return R(E(env)->NewWeakGlobalRef(O(obj)));
}

void JniEnvBackend::delete_weak_global_ref(Env env, Ref ref) {
    E(env)->DeleteWeakGlobalRef(static_cast<jweak>(O(ref)));
}

JniBackend::Ref JniEnvBackend::new_local_ref(Env env, Ref obj) {
    return R(E(env)->NewLocalRef(O(obj)));
}

void JniEnvBackend::delete_local_ref(Env env, Ref ref) {
    E(env)->DeleteLocalRef(O(ref));
}

std::int32_t JniEnvBackend::ensure_local_capacity(Env env, std::int32_t capacity) {
    return E(env)->EnsureLocalCapacity(capacity);
}

std::int32_t JniEnvBackend::push_local_frame(Env env, std::int32_t capacity) {
    return E(env)->PushLocalFrame(capacity);
}

JniBackend::Ref JniEnvBackend::pop_local_frame(Env env, Ref result) {
    return R(E(env)->PopLocalFrame(O(result)));
}

std::int32_t JniEnvBackend::throw_exception(Env env, Ref throwable) {
    return E(env)->Throw(static_cast<jthrowable>(O(throwable)));
}

std::int32_t JniEnvBackend::throw_new(Env env, Ref cls, const char* message) {
    return E(env)->ThrowNew(C(cls), message);
}

JniBackend::Ref JniEnvBackend::exception_occurred(Env env) {
    return R(E(env)->ExceptionOccurred());
}

void JniEnvBackend::exception_describe(Env env) {
    E(env)->ExceptionDescribe();
}

void JniEnvBackend::exception_clear(Env env) {
    E(env)->ExceptionClear();
}

bool JniEnvBackend::exception_check(Env env) {
    return E(env)->ExceptionCheck() == JNI_TRUE;
}

void JniEnvBackend::fatal_error(Env env, const char* message) {
    E(env)->FatalError(message);
}

std::int32_t JniEnvBackend::monitor_enter(Env env, Ref obj) {
    return E(env)->MonitorEnter(O(obj));
}

std::int32_t JniEnvBackend::monitor_exit(Env env, Ref obj) {
    return E(env)->MonitorExit(O(obj));
}

std::int32_t JniEnvBackend::register_native(Env env, Ref cls, const char* name, const char* signature,
                                            void* function) {
    const JNINativeMethod method = {name, signature, function};
    return E(env)->RegisterNatives(C(cls), &method, 1);
}

std::int32_t JniEnvBackend::unregister_natives(Env env, Ref cls) {
    return E(env)->UnregisterNatives(C(cls));
}

JniBackend::Ref JniEnvBackend::new_direct_byte_buffer(Env env, void* address, std::int64_t capacity) {
    return R(E(env)->NewDirectByteBuffer(address, capacity));
}

void* JniEnvBackend::get_direct_buffer_address(Env env, Ref buffer) {
    return E(env)->GetDirectBufferAddress(O(buffer));
}

std::int64_t JniEnvBackend::get_direct_buffer_capacity(Env env, Ref buffer) {
    return E(env)->GetDirectBufferCapacity(O(buffer));
}

JniBackend::Env JniEnvBackend::attach_current_thread(bool daemon, const char* name, Ref group) {
    JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>(name), O(group)};
    JNIEnv* env = nullptr;
    const jint rc = daemon ? vm_->AttachCurrentThreadAsDaemon(&env, &args) : vm_->AttachCurrentThread(&env, &args);
    return rc == JNI_OK ? static_cast<Env>(reinterpret_cast<std::uintptr_t>(env)) : 0;
}

std::int32_t JniEnvBackend::detach_current_thread() {
    return vm_->DetachCurrentThread();
}

}  // namespace zb
