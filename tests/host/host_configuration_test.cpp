#include <sys/mman.h>

#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_assets.h"
#include "mock_jvm.h"
#include "zb/guest_memory.h"
#include "zb/host_assets.h"
#include "zb/host_configuration.h"
#include "zb/host_jni.h"

namespace {
class MockConfiguration final : public zb::ConfigurationBackend {
public:
    struct Value { int orientation = 0, density = 0; char language[2] = {'e', 'n'}; };
    std::uint64_t create() override { const auto id = next_++; values_[id] = {}; return id; }
    void destroy(std::uint64_t id) override { values_.erase(id); }
    bool copy(std::uint64_t dest, std::uint64_t source) override {
        values_[dest] = values_.at(source);
        return true;
    }
    bool from_asset_manager(std::uint64_t id, std::uint64_t manager) override {
        CHECK(manager == MockAssetBackend::kManagerHandle);
        values_[id].density = 320;
        return true;
    }
    bool compare(const char* operation, std::uint64_t a, std::uint64_t b, std::int32_t& result) override {
        if (std::string(operation) != "diff") return false;
        result = values_.at(a).orientation != values_.at(b).orientation ? 1 : 0;
        return true;
    }
    bool get_int(const char* name, std::uint64_t id, std::int32_t& result) override {
        const std::string key(name);
        if (key == "getOrientation") result = values_.at(id).orientation;
        else if (key == "getDensity") result = values_.at(id).density;
        else return false;
        return true;
    }
    bool set_int(const char* name, std::uint64_t id, std::int32_t value) override {
        if (std::string(name) != "setOrientation") return false;
        values_.at(id).orientation = value;
        return true;
    }
    bool get_code(const char* name, std::uint64_t id, char code[2]) override {
        if (std::string(name) != "getLanguage") return false;
        std::memcpy(code, values_.at(id).language, 2);
        return true;
    }
    bool set_code(const char* name, std::uint64_t id, const char code[2]) override {
        if (std::string(name) != "setLanguage") return false;
        std::memcpy(values_.at(id).language, code, 2);
        return true;
    }
private:
    std::uint64_t next_ = 1;
    std::unordered_map<std::uint64_t, Value> values_;
};

std::int32_t call(zb::HostConfiguration& config, zb::GuestThread& thread, std::uint32_t index,
                  std::uint32_t a = 0, std::uint32_t b = 0) {
    thread.regs()[0] = a;
    thread.regs()[1] = b;
    CHECK(config.handle_host_call(index, thread));
    CHECK(thread.regs()[1] == 0);
    return static_cast<std::int32_t>(thread.regs()[0]);
}
}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    auto* vm = new zb::mock::MockJvm();
    auto* jni = new zb::HostJni(runtime, *vm);
    MockAssetBackend assets_backend;
    zb::HostAssets assets(runtime, assets_backend, *jni);
    MockConfiguration backend;
    zb::HostConfiguration config(runtime, backend, &assets);
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    CHECK(!config.handle_host_call(0, thread));
    const auto first = call(config, thread, 372);
    const auto second = call(config, thread, 372);
    CHECK(first != 0 && second != 0 && first != second);
    CHECK(call(config, thread, 384, first, 2) == 0);
    CHECK(call(config, thread, 359, first) == 2);
    CHECK(call(config, thread, 346, first, second) == 1);
    CHECK(call(config, thread, 344, second, first) == 0);
    CHECK(call(config, thread, 346, first, second) == 0);
    const char code[2] = {'f', 'r'};
    std::memcpy(runtime.memory().host_ptr(0x10000, 2, zb::kPageWrite), code, 2);
    CHECK(call(config, thread, 378, first, 0x10000) == 0);
    CHECK(call(config, thread, 353, first, 0x10010) == 0);
    CHECK(std::memcmp(runtime.memory().host_ptr(0x10010, 2, zb::kPageRead), code, 2) == 0);
    CHECK(call(config, thread, 353, first, 0xFFFF0000) == -EFAULT);

    thread.regs()[0] = 0;
    thread.regs()[1] = 0;
    CHECK(assets.handle_host_call(145, thread));
    const auto manager = thread.regs()[0];
    CHECK(manager != 0);
    CHECK(call(config, thread, 347, first, manager) == 0);
    CHECK(call(config, thread, 349, first) == 320);
    CHECK(call(config, thread, 345, first) == 0);
    CHECK(call(config, thread, 359, first) == -EINVAL);
    CHECK(call(config, thread, 345, second) == 0);
    std::puts("host_configuration_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
