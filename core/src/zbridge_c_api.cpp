#include "zb/zbridge.h"

#include <string>
#include <vector>

#include "zb/process.h"

extern char** environ;

int zb_run_executable(const char* sysroot, int argc, const char* const* argv, const char* const* envp) {
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) return 2;

    std::vector<std::string> guest_argv(argv, argv + argc);
    std::vector<std::string> guest_envp;
    if (envp != nullptr) {
        for (const char* const* e = envp; *e != nullptr; ++e) guest_envp.emplace_back(*e);
    } else {
        for (char** e = environ; *e != nullptr; ++e) guest_envp.emplace_back(*e);
    }

    zb::Process process;
    if (sysroot != nullptr) process.set_sysroot(sysroot);
    return process.run(guest_argv[0], guest_argv, guest_envp);
}
