#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "zb/gl_backend.h"

class MockGles final : public zb::GlBackend {
public:
    struct Call {
        std::string name;
        std::vector<std::uint64_t> arguments;
    };

    void set_error(zb::GLenum error) override { error_ = error; }
    zb::GLenum error() const { return error_; }
    const std::vector<Call>& calls() const { return calls_; }
    void clear_calls() { calls_.clear(); }
    void set_result(const std::string& name, std::uint64_t value) { results_[name] = value; }

protected:
    std::uint64_t invoke(const char* name,
                         std::initializer_list<std::uint64_t> arguments) override;

private:
    std::vector<Call> calls_;
    std::unordered_map<std::string, std::uint64_t> results_;
    std::uint32_t next_id_ = 1;
    zb::GLenum error_ = 0;
};
