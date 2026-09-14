#include "zb/elf_fixups.h"

#include <elf.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace zb {

namespace {

constexpr std::uint32_t kMaxDynamicSize = 64 * 1024;

bool read_exact(int fd, void* buf, std::size_t len, off_t offset) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pread(fd, static_cast<char*>(buf) + done, len - done, offset + static_cast<off_t>(done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

bool elf_has_textrel_marker(int fd) {
    Elf32_Ehdr eh;
    if (!read_exact(fd, &eh, sizeof eh, 0)) return false;
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_phentsize != sizeof(Elf32_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 64) {
        return false;
    }
    std::vector<Elf32_Phdr> phdrs(eh.e_phnum);
    if (!read_exact(fd, phdrs.data(), phdrs.size() * sizeof(Elf32_Phdr), eh.e_phoff)) return false;

    for (const auto& p : phdrs) {
        if (p.p_type != PT_DYNAMIC || p.p_filesz > kMaxDynamicSize) continue;
        std::vector<Elf32_Dyn> dyn(p.p_filesz / sizeof(Elf32_Dyn));
        if (!read_exact(fd, dyn.data(), dyn.size() * sizeof(Elf32_Dyn), p.p_offset)) return false;
        for (const auto& d : dyn) {
            if (d.d_tag == DT_NULL) break;
            if (d.d_tag == kDtZbTextrel) return true;
        }
    }
    return false;
}

}  // namespace zb
