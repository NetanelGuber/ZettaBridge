#include "zb/host_configuration.h"

#include <cerrno>
#include <cstring>
#include <string_view>

#include "zb/guest_memory.h"
#include "zb/host_assets.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {
namespace {
struct HostCall { std::uint32_t index; const char* library; const char* name; };
constexpr HostCall kHostCalls[] = {
#include "../gen/hostcalls.inc"
};

const char* config_name(std::uint32_t index) {
    for (const auto& call : kHostCalls) {
        if (call.index == index && std::string_view(call.library) == "libandroid.so" &&
            std::string_view(call.name).starts_with("AConfiguration_")) return call.name;
    }
    return nullptr;
}
}  // namespace

bool HostConfiguration::handle_host_call(std::uint32_t index, GuestThread& thread) {
    const char* full_name = config_name(index);
    if (full_name == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string_view name(full_name + std::strlen("AConfiguration_"));
    auto& regs = thread.regs();
    const auto host = [&](std::uint32_t handle) { return configs_.get(handle).value_or(0); };
    const auto unsupported = [&]() {
        runtime_report().note_unimplemented_host_call(index, "libandroid.so", full_name);
        log("%s is unavailable on this Android version", full_name);
    };
    const auto finish = [&](std::int32_t value) {
        regs[0] = static_cast<std::uint32_t>(value);
        regs[1] = 0;
        return true;
    };

    if (name == "new") {
        const auto value = backend_.create();
        const auto handle = value == 0 ? 0 : configs_.add(value);
        if (handle == 0 && value != 0) backend_.destroy(value);
        return finish(static_cast<std::int32_t>(handle));
    }
    if (name == "delete") {
        const auto value = configs_.remove(regs[0]);
        if (value && *value != 0) backend_.destroy(*value);
        return finish(0);
    }
    const auto first = host(regs[0]);
    if (first == 0) {
        log("%s received an invalid configuration handle", full_name);
        return finish(-EINVAL);
    }
    if (name == "copy") {
        const auto second = host(regs[1]);
        if (second == 0) log("AConfiguration_copy received an invalid source handle");
        else if (!backend_.copy(first, second)) unsupported();
        return finish(0);
    }
    if (name == "fromAssetManager") {
        const auto manager = assets_ == nullptr ? 0 : assets_->manager_value(regs[1]);
        if (manager == 0) log("AConfiguration_fromAssetManager received an invalid manager handle");
        else if (!backend_.from_asset_manager(first, manager)) unsupported();
        return finish(0);
    }
    if (name == "diff" || name == "match" || name == "isBetterThan") {
        const auto second = host(regs[1]);
        std::int32_t result = 0;
        if (second == 0) return finish(-EINVAL);
        if (!backend_.compare(name.data(), first, second, result)) {
            unsupported();
            return finish(-ENOSYS);
        }
        return finish(result);
    }
    if (name == "getLanguage" || name == "getCountry") {
        char* out = reinterpret_cast<char*>(runtime_.memory().host_ptr(regs[1], 2, kPageWrite));
        if (out == nullptr) return finish(-EFAULT);
        char code[2] = {};
        if (!backend_.get_code(full_name + std::strlen("AConfiguration_"), first, code)) unsupported();
        else std::memcpy(out, code, 2);
        return finish(0);
    }
    if (name == "setLanguage" || name == "setCountry") {
        const auto* source = runtime_.memory().host_ptr(regs[1], 2, kPageRead);
        if (source == nullptr) return finish(-EFAULT);
        if (!backend_.set_code(full_name + std::strlen("AConfiguration_"), first,
                               reinterpret_cast<const char*>(source))) unsupported();
        return finish(0);
    }
    if (name.starts_with("get")) {
        std::int32_t result = -ENOSYS;
        if (!backend_.get_int(full_name + std::strlen("AConfiguration_"), first, result)) unsupported();
        return finish(result);
    }
    if (name.starts_with("set")) {
        if (!backend_.set_int(full_name + std::strlen("AConfiguration_"), first,
                              static_cast<std::int32_t>(regs[1]))) unsupported();
        return finish(0);
    }
    unsupported();
    return finish(-ENOSYS);
}

}  // namespace zb
