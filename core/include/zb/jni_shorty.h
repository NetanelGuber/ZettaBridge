#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zb {

// Method shorty: the return type letter first, then one letter per parameter. Primitive types
// keep their descriptor letter (Z B C S I J F D, and V for the return); references and arrays
// are 'L'. Example: "(IFFIFF)I" -> "IIFFIFF". Returns nullopt for a malformed descriptor.
std::optional<std::string> shorty_from_signature(std::string_view signature);

}  // namespace zb
