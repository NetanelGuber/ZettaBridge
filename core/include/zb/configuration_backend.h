#pragma once

#include <cstdint>

namespace zb {

// Opaque host AConfiguration* and AAssetManager* values stay inside the host.
class ConfigurationBackend {
public:
    virtual ~ConfigurationBackend() = default;
    virtual std::uint64_t create() = 0;
    virtual void destroy(std::uint64_t config) = 0;
    virtual bool copy(std::uint64_t dest, std::uint64_t source) = 0;
    virtual bool from_asset_manager(std::uint64_t config, std::uint64_t manager) = 0;
    virtual bool compare(const char* operation, std::uint64_t a, std::uint64_t b,
                         std::int32_t& result) = 0;
    virtual bool get_int(const char* name, std::uint64_t config, std::int32_t& value) = 0;
    virtual bool set_int(const char* name, std::uint64_t config, std::int32_t value) = 0;
    virtual bool get_code(const char* name, std::uint64_t config, char code[2]) = 0;
    virtual bool set_code(const char* name, std::uint64_t config, const char code[2]) = 0;
};

}  // namespace zb
