#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zb {

// A native method named by a JNI export symbol "Java_<class>_<method>[__<arguments>]".
struct JniExport {
    std::string class_name;  // binary name with slashes, e.g. "org/haxe/lime/Lime"
    std::string method;      // e.g. "onTouch"
    // Argument part of the descriptor for overloaded exports, e.g. "(IFFIFF)"; no return type.
    std::optional<std::string> arguments;
};

// Returns nullopt when the symbol is not a well-formed JNI export name.
std::optional<JniExport> decode_jni_export(std::string_view symbol);

}  // namespace zb
