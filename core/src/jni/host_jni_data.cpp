// JNI host calls: strings, arrays, direct buffers.
#include "host_jni_internal.h"

#include <cstring>

#include "zb/log.h"

namespace zb {

namespace {

// Bytes of one element of a primitive type letter; 0 for anything else.
std::size_t element_size(char type) {
    switch (type) {
    case 'Z':
    case 'B':
        return 1;
    case 'C':
    case 'S':
        return 2;
    case 'I':
    case 'F':
        return 4;
    case 'J':
    case 'D':
        return 8;
    default:
        return 0;
    }
}

}  // namespace

bool HostJni::Impl::serve_data(JniCall& call) {
    JniThread& state = call.state();
    const char* name = jni_host_call_name(call.index());
    const auto ref = [&](unsigned position) { return resolve(state, call.arg(position), name); };
    const auto sint = [&](unsigned position) { return static_cast<std::int32_t>(call.arg(position)); };
    switch (call.index()) {
    case ZB_JNI_HC_NewString: {
        const JniBackend::Env env = call.env();
        const std::int32_t length = sint(1);
        const void* chars = length > 0 ? readable(env, call.arg(0), 2 * static_cast<std::uint64_t>(length), name) : nullptr;
        call.set(local(state, backend.new_string(env, static_cast<const std::uint16_t*>(chars), length)));
        return true;
    }
    case ZB_JNI_HC_NewStringUTF: {
        const JniBackend::Env env = call.env();
        if (call.arg(0) == 0) return true;  // ART returns null for a null string
        const std::string utf = read_string(env, call.arg(0), name);
        call.set(local(state, backend.new_string_utf(env, utf.c_str())));
        return true;
    }
    case ZB_JNI_HC_GetStringLength:
        call.set(static_cast<std::uint32_t>(backend.get_string_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetStringUTFLength:
        call.set(static_cast<std::uint32_t>(backend.get_string_utf_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetStringRegion: {
        const JniBackend::Env env = call.env();
        const JniBackend::Ref str = ref(0);
        const std::int32_t length = sint(2);
        void* out = length > 0 ? writable(env, call.arg(3), 2 * static_cast<std::uint64_t>(length), name) : nullptr;
        backend.get_string_region(env, str, sint(1), length, out);
        return true;
    }
    case ZB_JNI_HC_GetStringUTFRegion: {
        const JniBackend::Env env = call.env();
        const JniBackend::Ref str = ref(0);
        std::string utf;
        if (!backend.get_string_utf_region(env, str, sint(1), sint(2), utf)) return true;
        if (call.arg(3) == 0 && utf.empty()) return true;
        // ART writes the modified UTF-8 bytes and a NUL.
        std::memcpy(writable(env, call.arg(3), utf.size() + 1, name), utf.c_str(), utf.size() + 1);
        return true;
    }
    case ZB_JNI_HC_GetArrayLength:
        call.set(static_cast<std::uint32_t>(backend.get_array_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetArrayElementType:
        call.set(static_cast<std::uint8_t>(backend.get_array_element_type(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_NewObjectArray:
        call.set(local(state, backend.new_object_array(call.env(), sint(0), ref(1), ref(2))));
        return true;
    case ZB_JNI_HC_GetObjectArrayElement:
        call.set(local(state, backend.get_object_array_element(call.env(), ref(0), sint(1))));
        return true;
    case ZB_JNI_HC_SetObjectArrayElement:
        backend.set_object_array_element(call.env(), ref(0), sint(1), ref(2));
        return true;
    case ZB_JNI_HC_NewPrimitiveArray: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(0));
        if (element_size(type) == 0) fatal(env, "NewPrimitiveArray: bad type 0x%x", call.arg(0));
        call.set(local(state, backend.new_primitive_array(env, type, sint(1))));
        return true;
    }
    case ZB_JNI_HC_GetPrimitiveArrayRegion:
    case ZB_JNI_HC_SetPrimitiveArrayRegion: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(0));
        const std::size_t size = element_size(type);
        if (size == 0) fatal(env, "%s: bad type 0x%x", name, call.arg(0));
        const JniBackend::Ref array = ref(1);
        const std::int32_t length = sint(3);
        const std::uint64_t bytes = length > 0 ? static_cast<std::uint64_t>(length) * size : 0;
        if (call.index() == ZB_JNI_HC_GetPrimitiveArrayRegion) {
            void* out = bytes != 0 ? writable(env, call.arg(4), bytes, name) : nullptr;
            backend.get_primitive_array_region(env, type, array, sint(2), length, out);
        } else {
            const void* in = bytes != 0 ? readable(env, call.arg(4), bytes, name) : nullptr;
            backend.set_primitive_array_region(env, type, array, sint(2), length, in);
        }
        return true;
    }
    case ZB_JNI_HC_NewDirectByteBuffer: {
        const JniBackend::Env env = call.env();
        const std::uint32_t address = call.arg(0);
        const auto capacity = static_cast<std::int64_t>(call.arg(1) | (static_cast<std::uint64_t>(call.arg(2)) << 32));
        if (capacity > 0 && static_cast<std::uint64_t>(address) + static_cast<std::uint64_t>(capacity) > kGuestSpaceSize) {
            fatal(env, "NewDirectByteBuffer: 0x%08x + %lld leaves the guest address space", address,
                  static_cast<long long>(capacity));
        }
        void* host = address != 0 ? runtime.memory().base() + address : nullptr;
        call.set(local(state, backend.new_direct_byte_buffer(env, host, capacity)));
        return true;
    }
    case ZB_JNI_HC_GetDirectBufferAddress: {
        const JniBackend::Env env = call.env();
        const auto* host = static_cast<const std::uint8_t*>(backend.get_direct_buffer_address(env, ref(0)));
        const std::uint8_t* base = runtime.memory().base();
        if (host == nullptr) return true;
        if (host >= base && static_cast<std::uint64_t>(host - base) < kGuestSpaceSize) {
            call.set(static_cast<std::uint32_t>(host - base));
        } else if (!logged_foreign_buffer.exchange(true)) {
            log("GetDirectBufferAddress: the buffer lies outside guest memory; returning NULL (logged once)");
        }
        return true;
    }
    case ZB_JNI_HC_GetDirectBufferCapacity:
        call.set64(static_cast<std::uint64_t>(backend.get_direct_buffer_capacity(call.env(), ref(0))));
        return true;
    default:
        return false;
    }
}

}  // namespace zb
