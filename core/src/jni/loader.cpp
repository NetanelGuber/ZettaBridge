#include "zb/jni_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zb/elf_symbols.h"
#include "zb/host_jni.h"
#include "zb/jni_mangle.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::int32_t kJniVersion11 = 0x00010001;
constexpr std::int32_t kJniVersion12 = 0x00010002;
constexpr std::int32_t kJniVersion14 = 0x00010004;
constexpr std::int32_t kJniVersion16 = 0x00010006;

bool supported_version(std::int32_t version) {
    return version == kJniVersion11 || version == kJniVersion12 || version == kJniVersion14 ||
           version == kJniVersion16;
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool matches_arguments(const std::string& signature, const std::optional<std::string>& arguments) {
    if (!arguments) return true;
    const std::size_t close = signature.find(')');
    return close != std::string::npos && signature.compare(0, close + 1, *arguments) == 0;
}

class LocalClass {
public:
    LocalClass(JniBackend& backend, JniBackend::Env env, JniBackend::Ref ref)
        : backend_(backend), env_(env), ref_(ref) {}
    ~LocalClass() {
        if (ref_ != 0) backend_.delete_local_ref(env_, ref_);
    }
    LocalClass(const LocalClass&) = delete;
    LocalClass& operator=(const LocalClass&) = delete;

private:
    JniBackend& backend_;
    JniBackend::Env env_;
    JniBackend::Ref ref_;
};

}  // namespace

void JniLoader::log_missing_class_once(const std::string& name) {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    if (missing_classes_.insert(name).second) {
        log("JNI loader: class %s is not present; skipping its native exports", name.c_str());
    }
}

void JniLoader::log_unresolvable_once(const std::string& symbol, const std::string& name) {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    if (unresolvable_exports_.insert(symbol).second) {
        log("JNI loader: cannot resolve the declared natives of %s; skipping export %s", name.c_str(),
            symbol.c_str());
    }
}

JniLoadReport JniLoader::load(JniBackend::Env env, const std::string& path,
                              std::uint32_t guest_flags) {
    JniLoadReport report;
    const ElfSymbolReport symbols = scan_elf32_jni_exports(path);
    if (symbols.status != ElfSymbolStatus::Ok) {
        report.error = "cannot scan JNI exports: " + symbols.message;
        return report;
    }

    report.guest_handle = host_jni_.load_library_on_current(env, path, guest_flags, report.error);
    if (report.guest_handle == 0) {
        report.error = "guest dlopen failed: " + report.error;
        return report;
    }

    bool has_onload = false;
    for (const std::string& symbol : symbols.exports) {
        if (symbol == "JNI_OnLoad") {
            has_onload = true;
            continue;
        }
        const std::optional<JniExport> decoded = decode_jni_export(symbol);
        if (!decoded) {
            report.error = "invalid JNI export name: " + symbol;
            return report;
        }

        JniBackend::Ref cls = 0;
        std::vector<DeclaredNativeMethod> methods;
        const NativeLookupStatus lookup =
            backend_.find_declared_natives(env, decoded->class_name.c_str(), decoded->method.c_str(),
                                           decoded->arguments ? decoded->arguments->c_str() : nullptr, cls,
                                           methods);
        if (lookup == NativeLookupStatus::MissingClass) {
            log_missing_class_once(decoded->class_name);
            ++report.skipped_classes;
            continue;
        }
        // A type this export names, or a type of a sibling method the lookup had to touch, is not
        // present. That costs this one export, never the rest of the library: a guest that bundles
        // unresolvable classes next to the ones it needs must still bind the ones it needs.
        if (lookup == NativeLookupStatus::Unresolvable) {
            log_unresolvable_once(symbol, decoded->class_name);
            ++report.skipped_exports;
            continue;
        }
        if (lookup != NativeLookupStatus::Found || cls == 0) {
            report.error = "failed to inspect declared natives for " + decoded->class_name + "." + decoded->method;
            return report;
        }
        LocalClass local_class(backend_, env, cls);
        std::erase_if(methods, [&](const DeclaredNativeMethod& method) {
            return !matches_arguments(method.signature, decoded->arguments);
        });
        if (methods.empty()) {
            report.error = "no declared native matches export " + symbol;
            return report;
        }

        std::string symbol_error;
        const std::uint32_t function =
            host_jni_.find_symbol_on_current(env, report.guest_handle, symbol, symbol_error);
        if (function == 0) {
            report.error = "guest dlsym failed for " + symbol + ": " + symbol_error;
            return report;
        }
        for (const DeclaredNativeMethod& method : methods) {
            if (host_jni_.register_native(env, cls, decoded->method.c_str(), method.signature.c_str(), function,
                                          method.is_static) != 0) {
                report.error = "RegisterNatives failed for " + decoded->class_name + "." + decoded->method +
                               method.signature;
                return report;
            }
            ++report.bound_methods;
        }
    }

    if (has_onload) {
        const std::string library = base_name(path);
        std::string symbol_error;
        const std::uint32_t onload =
            host_jni_.find_symbol_on_current(env, report.guest_handle, "JNI_OnLoad", symbol_error);
        if (onload == 0) {
            report.error = "guest dlsym failed for JNI_OnLoad: " + symbol_error;
            return report;
        }
        const auto result = host_jni_.call_native(
            env, 'I', onload, [&](std::uint32_t, const RefToHandle&) {
                GuestCall call;
                call.regs = {host_jni_.guest_java_vm(), 0, 0, 0};
                return call;
            });
        if (!result) {
            runtime_report().note_jni_onload(library, false, 0);
            report.error = "guest JNI_OnLoad call failed";
            return report;
        }
        if (backend_.exception_check(env)) {
            runtime_report().note_jni_onload(library, false, 0);
            report.error = "guest JNI_OnLoad left a pending Java exception";
            return report;
        }
        report.jni_version = static_cast<std::int32_t>(result->guest.r0);
        if (!supported_version(report.jni_version)) {
            runtime_report().note_jni_onload(library, false, report.jni_version);
            report.error = "guest JNI_OnLoad returned unsupported JNI version 0x";
            char suffix[9];
            std::snprintf(suffix, sizeof suffix, "%08x", static_cast<std::uint32_t>(report.jni_version));
            report.error += suffix;
            return report;
        }
        runtime_report().note_jni_onload(library, true, report.jni_version);
    }

    report.ok = true;
    return report;
}

}  // namespace zb
