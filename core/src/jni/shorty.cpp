#include "zb/jni_shorty.h"

namespace zb {

namespace {

// Parses one type at pos and returns its shorty letter, or 0 when malformed. 'V' is accepted
// only where void_ok is set (the return type).
char parse_type(std::string_view s, std::size_t& pos, bool void_ok) {
    if (pos >= s.size()) return 0;
    switch (s[pos]) {
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
        return s[pos++];
    case 'V':
        if (!void_ok) return 0;
        ++pos;
        return 'V';
    case 'L': {
        const std::size_t end = s.find(';', pos);
        if (end == std::string_view::npos || end == pos + 1) return 0;
        pos = end + 1;
        return 'L';
    }
    case '[':
        while (pos < s.size() && s[pos] == '[') ++pos;
        return parse_type(s, pos, false) != 0 ? 'L' : 0;
    default:
        return 0;
    }
}

}  // namespace

std::optional<std::string> shorty_from_signature(std::string_view signature) {
    if (signature.empty() || signature[0] != '(') return std::nullopt;
    std::size_t pos = 1;
    std::string parameters;
    while (pos < signature.size() && signature[pos] != ')') {
        const char type = parse_type(signature, pos, false);
        if (type == 0) return std::nullopt;
        parameters.push_back(type);
    }
    if (pos >= signature.size()) return std::nullopt;
    ++pos;  // ')'
    const char return_type = parse_type(signature, pos, true);
    if (return_type == 0 || pos != signature.size()) return std::nullopt;
    return std::string(1, return_type) + parameters;
}

}  // namespace zb
