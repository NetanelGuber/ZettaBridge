#include <cstdio>
#include <string>
#include <vector>

#include "zb/process.h"

extern char** environ;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: zbrun <arm32-executable> [args...]\n");
        return 2;
    }
    std::vector<std::string> guest_argv(argv + 1, argv + argc);
    std::vector<std::string> guest_envp;
    for (char** e = environ; *e != nullptr; ++e) guest_envp.emplace_back(*e);

    zb::Process process;
    return process.run(argv[1], guest_argv, guest_envp);
}
