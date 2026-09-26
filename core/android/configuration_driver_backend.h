#pragma once

#include "zb/configuration_backend.h"

namespace zb {

class AndroidConfigurationBackend final : public ConfigurationBackend {
public:
    std::uint64_t create() override;
    void destroy(std::uint64_t config) override;
    bool copy(std::uint64_t dest, std::uint64_t source) override;
    bool from_asset_manager(std::uint64_t config, std::uint64_t manager) override;
    bool compare(const char* operation, std::uint64_t a, std::uint64_t b,
                 std::int32_t& result) override;
    bool get_int(const char* name, std::uint64_t config, std::int32_t& value) override;
    bool set_int(const char* name, std::uint64_t config, std::int32_t value) override;
    bool get_code(const char* name, std::uint64_t config, char code[2]) override;
    bool set_code(const char* name, std::uint64_t config, const char code[2]) override;
};

}  // namespace zb
