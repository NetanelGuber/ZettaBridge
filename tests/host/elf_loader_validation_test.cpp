#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "zb/elf_loader.h"

namespace {

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::size_t offset, const T& value) {
    CHECK(offset + sizeof value <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes(0x2000);
    Elf32_Ehdr eh{};
    std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_ARM;
    eh.e_version = EV_CURRENT;
    eh.e_ehsize = sizeof eh;
    eh.e_phoff = sizeof eh;
    eh.e_phentsize = sizeof(Elf32_Phdr);
    eh.e_phnum = 2;
    eh.e_entry = 0x1100;
    put(bytes, 0, eh);

    Elf32_Phdr text{};
    text.p_type = PT_LOAD;
    text.p_offset = 0;
    text.p_vaddr = 0x1000;
    text.p_filesz = 0x200;
    text.p_memsz = 0x1000;
    text.p_flags = PF_R | PF_X;
    text.p_align = 0x1000;
    put(bytes, eh.e_phoff, text);

    Elf32_Phdr data{};
    data.p_type = PT_LOAD;
    data.p_offset = 0x1000;
    data.p_vaddr = 0x5000;
    data.p_filesz = 4;
    data.p_memsz = 0x1000;
    data.p_flags = PF_R | PF_W;
    data.p_align = 0x1000;
    put(bytes, eh.e_phoff + sizeof text, data);
    return bytes;
}

bool load(const std::vector<std::uint8_t>& bytes, zb::GuestMemory& memory, zb::LoadedElf& elf,
          std::string& error) {
    char name[] = "/tmp/zb-elf-loader-XXXXXX";
    const int fd = ::mkstemp(name);
    CHECK(fd >= 0);
    CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    CHECK(::close(fd) == 0);
    const bool result = zb::load_elf(memory, name, 0x10000000, elf, error);
    ::unlink(name);
    return result;
}

}  // namespace

int main() {
    {
        zb::GuestMemory memory;
        zb::LoadedElf elf;
        std::string error;
        CHECK(load(fixture(), memory, elf, error));
        CHECK(memory.accessible(elf.entry, 1, zb::kPageExec));
        CHECK(!memory.accessible(elf.entry, 1, zb::kPageWrite));
        CHECK(!memory.accessible(elf.load_start + 0x2000, 1, 0));
        CHECK(memory.accessible(elf.load_start + 0x4000, 1, zb::kPageWrite));
    }
    auto bad = fixture();
    Elf32_Ehdr eh;
    Elf32_Phdr ph;
    std::memcpy(&eh, bad.data(), sizeof eh);
    std::memcpy(&ph, bad.data() + eh.e_phoff, sizeof ph);
    {
        zb::GuestMemory memory;
        zb::LoadedElf elf;
        std::string error;
        ph.p_vaddr = 0xfffff000;
        put(bad, eh.e_phoff, ph);
        CHECK(!load(bad, memory, elf, error));
        CHECK(error.find("bad PT_LOAD") != std::string::npos);
    }
    bad = fixture();
    std::memcpy(&ph, bad.data() + eh.e_phoff, sizeof ph);
    ph.p_align = 3;
    put(bad, eh.e_phoff, ph);
    {
        zb::GuestMemory memory;
        zb::LoadedElf elf;
        std::string error;
        CHECK(!load(bad, memory, elf, error));
    }
    bad = fixture();
    eh.e_entry = 0x3000;
    put(bad, 0, eh);
    {
        zb::GuestMemory memory;
        zb::LoadedElf elf;
        std::string error;
        CHECK(!load(bad, memory, elf, error));
        CHECK(error.find("entry") != std::string::npos);
    }
    bad = fixture();
    eh.e_entry = 0x1100;
    eh.e_phoff = 0x1ff0;
    put(bad, 0, eh);
    {
        zb::GuestMemory memory;
        zb::LoadedElf elf;
        std::string error;
        CHECK(!load(bad, memory, elf, error));
        CHECK(error.find("program header") != std::string::npos);
    }
    std::puts("elf_loader_validation_test PASS");
}
