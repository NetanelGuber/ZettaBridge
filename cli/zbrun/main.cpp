#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "zb/process.h"

extern char** environ;

namespace {

int usage() {
    std::fprintf(stderr, "usage: zbrun [--sysroot DIR] <arm32-executable> [args...]\n"
                         "  --sysroot DIR  arm32 Android system files (default: $ZB_SYSROOT)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string sysroot;
    if (const char* env = std::getenv("ZB_SYSROOT")) sysroot = env;

    int i = 1;
    while (i < argc && std::strncmp(argv[i], "--", 2) == 0) {
        if (std::strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) {
            sysroot = argv[i + 1];
            i += 2;
        } else {
            return usage();
        }
    }
    if (i >= argc) return usage();

    std::vector<std::string> guest_argv(argv + i, argv + argc);
    std::vector<std::string> guest_envp;
    for (char** e = environ; *e != nullptr; ++e) guest_envp.emplace_back(*e);

    zb::Process process;
    process.set_sysroot(sysroot);
    return process.run(argv[i], guest_argv, guest_envp);
}
