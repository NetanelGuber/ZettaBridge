#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "zb/process.h"

extern char** environ;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: zbrun [--sysroot DIR] [--env NAME=VALUE]... <arm32-executable> [args...]\n"
                 "  --sysroot DIR     arm32 Android system files (default: $ZB_SYSROOT)\n"
                 "  --env NAME=VALUE  set a variable for the guest only; the host dynamic loader never\n"
                 "                    sees it (use for guest LD_DEBUG, LD_LIBRARY_PATH, ...)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string sysroot;
    if (const char* env = std::getenv("ZB_SYSROOT")) sysroot = env;

    std::vector<std::string> guest_envp;
    for (char** e = environ; *e != nullptr; ++e) guest_envp.emplace_back(*e);

    int i = 1;
    while (i < argc && std::strncmp(argv[i], "--", 2) == 0) {
        if (std::strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) {
            sysroot = argv[i + 1];
            i += 2;
        } else if (std::strcmp(argv[i], "--env") == 0 && i + 1 < argc && std::strchr(argv[i + 1], '=') != nullptr) {
            const std::string assignment = argv[i + 1];
            const std::string prefix = assignment.substr(0, assignment.find('=') + 1);
            std::erase_if(guest_envp, [&](const std::string& e) { return e.compare(0, prefix.size(), prefix) == 0; });
            guest_envp.push_back(assignment);
            i += 2;
        } else {
            return usage();
        }
    }
    if (i >= argc) return usage();

    std::vector<std::string> guest_argv(argv + i, argv + argc);

    zb::Process process;
    process.set_sysroot(sysroot);
    return process.run(argv[i], guest_argv, guest_envp);
}
