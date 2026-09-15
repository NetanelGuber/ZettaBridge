// HostJni core: API registration, per-thread JNI state, handle and id resolution, guest memory
// access, native calls, and the host-call switch.
#include "host_jni_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "zb/log.h"
#include "zb/process.h"

namespace zb {

namespace {

struct JniHostCallName {
    std::uint32_t index;
    const char* name;
};

constexpr JniHostCallName kJniHostCallNames[] = {
#include "gen/jni_hostcalls.inc"
};

// Longest guest string read by a host call (class names, signatures, UTF text).
constexpr std::uint64_t kMaxGuestString = 64u << 20;

thread_local JniThread t_thread;

}  // namespace

const char* jni_host_call_name(std::uint32_t index) {
    for (const auto& entry : kJniHostCallNames) {
        if (entry.index == index) return entry.name;
    }
    return "?";
}

JniThread::~JniThread() {
    // A Java thread that ran guest natives frees its guest JNIEnv on its carrier before the lease
    // ends. Guest threads free theirs in DetachCurrentThread.
    if (owner != nullptr && carrier && guest_env != 0) {
        GuestCall args;
        args.regs = {guest_env, 0, 0, 0};
        (void)carrier->call(owner->api.free_env_fn, args);
        guest_env = 0;
    }
}

std::uint32_t JniCall::arg(unsigned position) const {
    if (position < 4) return regs_[position];
    const std::uint32_t address = thread_.regs()[13] + 4 * (position - 4);
    const std::uint8_t* word = jni_.runtime.memory().host_ptr(address, 4, kPageRead);
    if (word == nullptr) {
        jni_.fatal(state_.env, "JNI %s: argument %u is not on a readable guest stack", jni_host_call_name(index_),
                   position);
    }
    std::uint32_t value;
    std::memcpy(&value, word, sizeof value);
    return value;
}

JniBackend::Env JniCall::env() {
    if (state_.env == 0) {
        log("JNI %s called on a thread without a JNIEnv", jni_host_call_name(index_));
        std::abort();
    }
    return state_.env;
}

JniThread& HostJni::Impl::thread() {
    JniThread& state = t_thread;
    state.owner = this;
    return state;
}

void HostJni::Impl::fatal(JniBackend::Env env, const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log("JNI fatal error: %s", text);
    if (env != 0) backend.fatal_error(env, text);
    std::abort();
}

JniBackend::Ref HostJni::Impl::resolve(JniThread& state, std::uint32_t handle, const char* function) {
    if (handle == 0) return 0;
    std::optional<std::uint64_t> ref;
    switch (handle & 3u) {
    case static_cast<std::uint32_t>(HandleKind::Local):
        ref = state.locals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::Global):
        ref = globals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::WeakGlobal):
        ref = weaks.get(handle);
        break;
    default:
        break;
    }
    if (!ref) fatal(state.env, "%s: invalid JNI reference 0x%08x", function, handle);
    return *ref;
}

std::uint32_t HostJni::Impl::intern_method(JniBackend::Id id, const std::string& shorty) {
    const std::uint32_t guest = methods.intern(id);
    if (guest == 0) return 0;
    std::lock_guard<std::mutex> lock(shorty_mutex);
    if (shorties.size() < guest) shorties.resize(guest);
    shorties[guest - 1] = shorty;
    return guest;
}

std::optional<std::string> HostJni::Impl::method_shorty(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(shorty_mutex);
    if (id == 0 || id > shorties.size() || shorties[id - 1].empty()) return std::nullopt;
    return shorties[id - 1];
}

JniBackend::Id HostJni::Impl::method_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = methods.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jmethodID 0x%08x", function, id);
    return *host;
}

JniBackend::Id HostJni::Impl::field_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = fields.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jfieldID 0x%08x", function, id);
    return *host;
}

std::string HostJni::Impl::read_string(JniBackend::Env env, std::uint32_t address, const char* function) {
    std::string text;
    std::uint64_t cursor = address;
    for (;;) {
        // Read up to the end of the current page, which is mapped as a whole or not at all.
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        if (cursor >= kGuestSpaceSize || text.size() > kMaxGuestString) {
            fatal(env, "%s: unterminated guest string at 0x%08x", function, address);
        }
        const std::uint8_t* bytes =
            runtime.memory().host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) fatal(env, "%s: unreadable guest string at 0x%08x", function, address);
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
}

const std::uint8_t* HostJni::Impl::readable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                            const char* function) {
    const std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageRead);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unreadable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

std::uint8_t* HostJni::Impl::writable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                      const char* function) {
    std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageWrite);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unwritable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

void HostJni::Impl::write_shorty(JniBackend::Env env, std::uint32_t address, const std::string& shorty,
                                 const char* function) {
    if (shorty.size() + 1 > ZB_JNI_SHORTY_SIZE) fatal(env, "%s: shorty longer than 256 letters", function);
    std::memcpy(writable(env, address, shorty.size() + 1, function), shorty.c_str(), shorty.size() + 1);
}

std::optional<GuestResult> HostJni::Impl::invoke(JniThread& state, std::uint32_t function, const GuestCall& args) {
    if (Process::current_thread() != nullptr) return runtime.call_on_current(function, args);
    if (!state.carrier) {
        std::string error;
        state.carrier = runtime.borrow(error);
        if (!state.carrier) {
            log("JNI: cannot borrow a guest carrier: %s", error.c_str());
            return std::nullopt;
        }
    }
    return state.carrier->call(function, args);
}

bool HostJni::Impl::ensure_guest_env(JniThread& state) {
    if (state.guest_env != 0) return true;
    const std::optional<GuestResult> env = invoke(state, api.new_env_fn, GuestCall{});
    if (!env || env->r0 == 0) {
        log("JNI: guest JNIEnv allocation failed");
        return false;
    }
    state.guest_env = env->r0;
    return true;
}

HostJni::HostJni(LibraryRuntime& runtime, JniBackend& backend)
    : impl_(std::make_unique<Impl>(runtime, backend)) {
    impl_->owner = this;
}

HostJni::~HostJni() {
    log("HostJni destroyed; the JNI bridge is process-lifetime");
    std::abort();
}

bool HostJni::ready() const {
    return impl_->ready.load();
}

std::uint32_t HostJni::guest_java_vm() const {
    return impl_->api.java_vm;
}

bool HostJni::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_JNI_SLOT_STUB_FIRST || index > ZB_JNI_HOST_CALL_LAST) return false;
    Impl& jni = *impl_;
    if (index == ZB_JNI_HC_Register) {
        zb_jni_guest_api api{};
        const std::uint8_t* source = jni.runtime.memory().host_ptr(thread.regs()[0], sizeof api, kPageRead);
        if (source != nullptr) std::memcpy(&api, source, sizeof api);
        const bool valid = source != nullptr && api.size == sizeof api && api.version == ZB_JNI_PROTOCOL_VERSION &&
                           api.new_env_fn != 0 && api.free_env_fn != 0 && api.java_vm != 0;
        if (valid && !jni.ready.load()) {
            jni.api = api;
            jni.ready.store(true);
        } else if (!valid) {
            log("libzbjni.so registered an invalid or mismatched API");
        }
        thread.regs()[0] = valid ? 1 : 0;
        return true;
    }
    if (!jni.ready.load()) {
        log("JNI host call %s before libzbjni.so registered", jni_host_call_name(index));
        std::abort();
    }
    JniCall call(jni, thread, jni.thread(), index);
    if (jni.serve_objects(call)) {
        return true;
    }
    log("JNI host call 0x%x (%s) is not implemented", index, jni_host_call_name(index));
    std::abort();
}

std::optional<HostJni::NativeResult> HostJni::call_native(JniBackend::Env env, char return_type,
                                                          std::uint32_t function, const BuildCall& build) {
    Impl& jni = *impl_;
    JniThread& state = jni.thread();
    const JniBackend::Env outer_env = state.env;
    if (outer_env != 0 && outer_env != env) {
        log("JNI native call with a JNIEnv that differs from this thread's JNIEnv");
        std::abort();
    }
    if (!jni.ready.load() || !jni.ensure_guest_env(state)) return std::nullopt;
    // The backend frame frees every host local created during the call in one step and keeps
    // the caller's (argument) references untouched.
    if (jni.backend.push_local_frame(env, 16) != 0) return std::nullopt;
    state.env = env;
    ++state.native_depth;
    const int outer_user_frames = state.user_frames;
    state.user_frames = 0;
    state.locals.push_frame();
    const std::size_t frames = state.locals.frame_count();

    const GuestCall args =
        build(state.guest_env, [&](std::uint64_t ref) { return state.locals.add(ref); });
    std::optional<GuestResult> guest = jni.invoke(state, function, args);

    std::optional<NativeResult> result;
    JniBackend::Ref kept = 0;
    if (guest) {
        result = NativeResult{*guest, 0};
        if (return_type == 'L') kept = jni.resolve(state, guest->r0, "native method result");
    }
    if (state.locals.frame_count() != frames || state.user_frames != 0) {
        log("JNI: a native method returned with %d unpopped PushLocalFrame frames", state.user_frames);
        while (state.user_frames > 0) {
            state.locals.pop_frame();
            kept = jni.backend.pop_local_frame(env, kept);
            --state.user_frames;
        }
    }
    state.locals.pop_frame();
    const JniBackend::Ref outer = jni.backend.pop_local_frame(env, kept);
    if (result && return_type == 'L') result->ref = outer;
    state.user_frames = outer_user_frames;
    --state.native_depth;
    state.env = outer_env;
    return result;
}

}  // namespace zb
