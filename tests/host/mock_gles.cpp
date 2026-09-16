#include "mock_gles.h"

#include <algorithm>
#include <cstring>

std::uint64_t MockGles::invoke(const char* name,
                               std::initializer_list<std::uint64_t> arguments) {
    calls_.push_back(Call{name, std::vector<std::uint64_t>(arguments)});
    const auto& args = calls_.back().arguments;
    if (calls_.back().name == "glGetIntegerv" && args.size() == 2 && args[1] != 0) {
        const auto value = integers_.find(static_cast<zb::GLenum>(args[0]));
        if (value != integers_.end()) *reinterpret_cast<zb::GLint*>(args[1]) = value->second;
    } else if (calls_.back().name == "glGetProgramiv" && args.size() == 3 && args[2] != 0 &&
               (args[1] == 0x8B86 || args[1] == 0x8B87)) {
        const auto& uniforms = uniforms_[static_cast<zb::GLuint>(args[0])];
        if (args[1] == 0x8B86) {  // GL_ACTIVE_UNIFORMS
            *reinterpret_cast<zb::GLint*>(args[2]) = static_cast<zb::GLint>(uniforms.size());
        } else {  // GL_ACTIVE_UNIFORM_MAX_LENGTH
            std::size_t longest = 1;
            for (const ActiveUniform& uniform : uniforms) longest = std::max(longest, uniform.name.size() + 1);
            *reinterpret_cast<zb::GLint*>(args[2]) = static_cast<zb::GLint>(longest);
        }
    } else if (calls_.back().name == "glGetActiveUniform" && args.size() == 7) {
        auto& uniforms = uniforms_[static_cast<zb::GLuint>(args[0])];
        const std::size_t index = static_cast<std::size_t>(args[1]);
        if (index < uniforms.size()) {
            const ActiveUniform& uniform = uniforms[index];
            const auto capacity = static_cast<std::size_t>(static_cast<zb::GLsizei>(args[2]));
            const std::size_t copied = capacity == 0 ? 0 : std::min(capacity - 1, uniform.name.size());
            if (args[3] != 0) *reinterpret_cast<zb::GLsizei*>(args[3]) = static_cast<zb::GLsizei>(copied);
            if (args[4] != 0) *reinterpret_cast<zb::GLint*>(args[4]) = uniform.size;
            if (args[5] != 0) *reinterpret_cast<zb::GLenum*>(args[5]) = uniform.type;
            if (args[6] != 0 && capacity != 0) {
                std::memcpy(reinterpret_cast<void*>(args[6]), uniform.name.data(), copied);
                reinterpret_cast<char*>(args[6])[copied] = '\0';
            }
        }
    } else if (calls_.back().name == "glGetUniformLocation" && args.size() == 2 && args[1] != 0) {
        const auto found = uniforms_.find(static_cast<zb::GLuint>(args[0]));
        if (found != uniforms_.end()) {
            for (const ActiveUniform& uniform : found->second) {
                std::string lookup_name = uniform.name;
                if (lookup_name.ends_with("[0]")) lookup_name.resize(lookup_name.size() - 3);
                if (lookup_name == reinterpret_cast<const char*>(args[1])) {
                    return static_cast<std::uint64_t>(uniform.location);
                }
            }
        }
        return static_cast<std::uint64_t>(-1);
    }
    const auto found = results_.find(name);
    if (found != results_.end()) return found->second;
    if (calls_.back().name == "glCreateProgram" || calls_.back().name == "glCreateShader") {
        return next_id_++;
    }
    return 0;
}
