#pragma once

#include <cstdint>

namespace zb {

// Dynamic tag written by tools/fix_guest_lib.py in place of DT_TEXTREL. The guest linker
// ignores it (it is in the OS-specific range); zbrun keeps the library's code pages writable
// so the linker's text relocations succeed.
inline constexpr std::int32_t kDtZbTextrel = 0x60005A42;

// True if the ELF32 file behind fd has a DT_ZB_TEXTREL entry in its dynamic section.
bool elf_has_textrel_marker(int fd);

}  // namespace zb
