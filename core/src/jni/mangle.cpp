// Decoding of JNI export names (JNI specification, "Resolving Native Method Names").
#include "zb/jni_mangle.h"

namespace zb {

namespace {

// Appends one UTF-16 code unit as UTF-8 (surrogates stay separate, as in modified UTF-8).
void append_utf8(std::string& out, unsigned unit) {
    if (unit < 0x80) {
        out.push_back(static_cast<char>(unit));
    } else if (unit < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes from pos until the end or a "__" separator (then hit_separator is set and pos is past
// it). A plain '_' becomes '/'. Returns false for a malformed escape or a trailing '_'.
bool decode_part(std::string_view in, std::size_t& pos, std::string& out, bool& hit_separator) {
    hit_separator = false;
    while (pos < in.size()) {
        const char c = in[pos];
        if (c != '_') {
            out.push_back(c);
            ++pos;
            continue;
        }
        if (pos + 1 >= in.size()) return false;
        switch (in[pos + 1]) {
        case '1':
            out.push_back('_');
            pos += 2;
            break;
        case '2':
            out.push_back(';');
            pos += 2;
            break;
        case '3':
            out.push_back('[');
            pos += 2;
            break;
        case '0': {
            if (pos + 6 > in.size()) return false;
            unsigned unit = 0;
            for (std::size_t i = pos + 2; i < pos + 6; ++i) {
                const int v = hex_value(in[i]);
                if (v < 0) return false;
                unit = unit * 16 + static_cast<unsigned>(v);
            }
            append_utf8(out, unit);
            pos += 6;
            break;
        }
        case '_':
            hit_separator = true;
            pos += 2;
            return true;
        default:
            out.push_back('/');
            ++pos;
            break;
        }
    }
    return true;
}

}  // namespace

std::optional<JniExport> decode_jni_export(std::string_view symbol) {
    constexpr std::string_view kPrefix = "Java_";
    if (symbol.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    std::size_t pos = kPrefix.size();

    std::string path;
    bool separator = false;
    if (!decode_part(symbol, pos, path, separator)) return std::nullopt;
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == path.size()) return std::nullopt;

    JniExport result;
    result.class_name = path.substr(0, slash);
    result.method = path.substr(slash + 1);
    if (separator) {
        std::string arguments;
        bool again = false;
        if (!decode_part(symbol, pos, arguments, again) || again) return std::nullopt;
        result.arguments = "(" + arguments + ")";
    }
    return result;
}

}  // namespace zb
