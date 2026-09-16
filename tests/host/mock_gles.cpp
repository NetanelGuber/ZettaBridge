#include "mock_gles.h"

std::uint64_t MockGles::invoke(const char* name,
                               std::initializer_list<std::uint64_t> arguments) {
    calls_.push_back(Call{name, std::vector<std::uint64_t>(arguments)});
    const auto found = results_.find(name);
    if (found != results_.end()) return found->second;
    if (calls_.back().name == "glCreateProgram" || calls_.back().name == "glCreateShader") {
        return next_id_++;
    }
    return 0;
}
