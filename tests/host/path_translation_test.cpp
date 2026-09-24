// Executable guest system paths stay in the ARM32 sysroot. Missing font data may use the device.
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/mman.h>

#include "check.h"
#include "zb/process.h"

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "zb-path-translation-test";
    fs::remove_all(root);
    fs::create_directories(root / "system" / "lib");
    std::FILE* library = std::fopen((root / "system" / "lib" / "libc.so").c_str(), "w");
    CHECK(library != nullptr);
    std::fputs("not a real library", library);
    std::fclose(library);

    zb::Process process;
    process.set_sysroot(root.string());

    // A file the sysroot has: the guest gets the sysroot copy, never the device's own.
    CHECK(process.translate_path("/system/lib/libc.so") == (root / "system/lib/libc.so").string());
    // The APEX bionic path collapses onto the same copy.
    CHECK(process.translate_path("/apex/com.android.runtime/lib/bionic/libc.so") ==
          (root / "system/lib/libc.so").string());

    // A file the sysroot does not have: the guest gets the device path unchanged. Fonts are the
    // reason this rule exists; they are architecture-independent and live outside the sysroot.
    CHECK(process.translate_path("/system/fonts/Roboto-Regular.ttf") ==
          std::string("/system/fonts/Roboto-Regular.ttf"));
    CHECK(process.translate_path("/system/etc/fonts.xml") == std::string("/system/etc/fonts.xml"));
    CHECK(process.translate_path("/system/lib/libmissing.so") ==
          (root / "system/lib/libmissing.so").string());
    CHECK(process.translate_path("/apex/com.android.runtime/lib/bionic/libmissing.so") ==
          (root / "system/lib/libmissing.so").string());
    CHECK(process.translate_path("/vendor/lib/libmissing.so") ==
          (root / "vendor/lib/libmissing.so").string());
    CHECK(process.translate_path("/system/lib/../../lib64/libhost.so") ==
          (root / ".zb-denied").string());
    fs::create_directory_symlink("/usr/lib", root / "system/lib/escape");
    CHECK(process.translate_path("/system/lib/escape/libhost.so") ==
          (root / ".zb-denied").string());

    Elf32_Ehdr elf{};
    std::memcpy(elf.e_ident, ELFMAG, SELFMAG);
    elf.e_ident[EI_CLASS] = ELFCLASS32;
    elf.e_ident[EI_DATA] = ELFDATA2LSB;
    elf.e_type = ET_DYN;
    elf.e_machine = EM_ARM;
    const fs::path inside = root / "system/lib/libarm.so";
    const fs::path outside = fs::temp_directory_path() / "zb-outside-arm.so";
    for (const auto& path : {inside, outside}) {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        CHECK(file != nullptr && std::fwrite(&elf, 1, sizeof elf, file) == sizeof elf);
        CHECK(std::fclose(file) == 0);
    }
    const int guest_fd = ::open(inside.c_str(), O_RDONLY);
    const int host_fd = ::open(outside.c_str(), O_RDONLY);
    CHECK(guest_fd >= 0 && host_fd >= 0);
    CHECK(process.guest_executable_allowed(guest_fd));
    CHECK(!process.guest_executable_allowed(host_fd));
    CHECK(::close(guest_fd) == 0 && ::close(host_fd) == 0);
    process.set_exec_eligibility(0x10000, 0x2000, false);
    CHECK(!process.may_execute_range(0x10000, 0x1000));
    CHECK(process.may_execute_range(0x12000, 0x1000));
    process.set_exec_eligibility(0x10000, 0x2000, true);
    CHECK(process.may_execute_range(0x10000, 0x2000));
    CHECK(process.memory().map_anon(0x20000, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC));
    process.add_textrel_range(0x20000, 0x1000, PROT_READ | PROT_EXEC);
    CHECK(process.seal_textrel_ranges());
    CHECK(process.memory().accessible(0x20000, 1, zb::kPageExec));
    CHECK(!process.memory().accessible(0x20000, 1, zb::kPageWrite));
    fs::remove(outside);

    // Paths outside the mapped prefixes are never touched.
    CHECK(process.translate_path("/data/data/com.example/files/thing.ttf") ==
          std::string("/data/data/com.example/files/thing.ttf"));

    fs::remove_all(root);
    std::puts("path_translation_test PASS");
    return 0;
}
