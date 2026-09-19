// Guest system paths map into the sysroot, which holds only the 32-bit binaries. Anything the
// sysroot does not have - fonts above all - must fall through to the device's own file, or a
// guest can load its libraries and still be unable to draw a single letter (Flutter, 2026-09-19).
#include <cstdio>
#include <filesystem>
#include <string>

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

    // Paths outside the mapped prefixes are never touched.
    CHECK(process.translate_path("/data/data/com.example/files/thing.ttf") ==
          std::string("/data/data/com.example/files/thing.ttf"));

    fs::remove_all(root);
    std::puts("path_translation_test PASS");
    return 0;
}
