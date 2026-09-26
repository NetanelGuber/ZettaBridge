#include "configuration_driver_backend.h"

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <dlfcn.h>

#include <cstdint>
#include <string>

namespace zb {
namespace {
template <typename Function>
Function resolve(const std::string& name) {
    // Newer configuration properties do not exist on every Android version. Resolve at use
    // time so an API-29 zbridge still links and a missing property is reported explicitly.
    return reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, name.c_str()));
}
AConfiguration* C(std::uint64_t handle) {
    return reinterpret_cast<AConfiguration*>(static_cast<std::uintptr_t>(handle));
}
}  // namespace

std::uint64_t AndroidConfigurationBackend::create() {
    return reinterpret_cast<std::uint64_t>(AConfiguration_new());
}
void AndroidConfigurationBackend::destroy(std::uint64_t config) { AConfiguration_delete(C(config)); }
bool AndroidConfigurationBackend::copy(std::uint64_t dest, std::uint64_t source) {
    AConfiguration_copy(C(dest), C(source));
    return true;
}
bool AndroidConfigurationBackend::from_asset_manager(std::uint64_t config, std::uint64_t manager) {
    AConfiguration_fromAssetManager(C(config), reinterpret_cast<AAssetManager*>(static_cast<std::uintptr_t>(manager)));
    return true;
}
bool AndroidConfigurationBackend::compare(const char* operation, std::uint64_t a, std::uint64_t b,
                                           std::int32_t& result) {
    using Function = std::int32_t (*)(AConfiguration*, AConfiguration*);
    const auto function = resolve<Function>(std::string("AConfiguration_") + operation);
    if (function == nullptr) return false;
    result = function(C(a), C(b));
    return true;
}
bool AndroidConfigurationBackend::get_int(const char* name, std::uint64_t config, std::int32_t& value) {
    using Function = std::int32_t (*)(AConfiguration*);
    const auto function = resolve<Function>(std::string("AConfiguration_") + name);
    if (function == nullptr) return false;
    value = function(C(config));
    return true;
}
bool AndroidConfigurationBackend::set_int(const char* name, std::uint64_t config, std::int32_t value) {
    using Function = void (*)(AConfiguration*, std::int32_t);
    const auto function = resolve<Function>(std::string("AConfiguration_") + name);
    if (function == nullptr) return false;
    function(C(config), value);
    return true;
}
bool AndroidConfigurationBackend::get_code(const char* name, std::uint64_t config, char code[2]) {
    using Function = void (*)(AConfiguration*, char*);
    const auto function = resolve<Function>(std::string("AConfiguration_") + name);
    if (function == nullptr) return false;
    function(C(config), code);
    return true;
}
bool AndroidConfigurationBackend::set_code(const char* name, std::uint64_t config, const char code[2]) {
    using Function = void (*)(AConfiguration*, const char*);
    const auto function = resolve<Function>(std::string("AConfiguration_") + name);
    if (function == nullptr) return false;
    function(C(config), code);
    return true;
}

}  // namespace zb
