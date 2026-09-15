#pragma once

#include <jni.h>

#include <string>

#include "jni_env_backend.h"
#include "zb/proxy_runtime.h"

namespace zb {

// The one guest JNI runtime of an app process (Android build only):
//   JniEnvBackend -> GuestJniEngine (LibraryRuntime + HostJni + JniLoader) -> ProxyRuntime.
// Created on first use and intentionally never destroyed: guest threads, bound native thunks and
// the retained plugin class loader live as long as the process.
class GuestJniRuntime {
public:
    static GuestJniRuntime& get(JNIEnv* env);
    // The runtime if get() already created it, else nullptr (error queries never create it).
    static GuestJniRuntime* peek();

    ProxyRuntime& proxies() { return proxies_; }

private:
    class Engine final : public GuestJniEngine {
    public:
        explicit Engine(JniEnvBackend& backend) : GuestJniEngine(backend), jni_backend_(backend) {}
        bool bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) override;

    protected:
        std::string take_pending_exception(JniBackend::Env env) override;

    private:
        JniEnvBackend& jni_backend_;
    };

    explicit GuestJniRuntime(JavaVM* vm);

    JniEnvBackend backend_;
    Engine engine_;
    ProxyRuntime proxies_;
};

}  // namespace zb
