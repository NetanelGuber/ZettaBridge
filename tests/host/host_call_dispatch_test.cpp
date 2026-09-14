#include <string>

#include "check.h"
#include "zb/process.h"

int main(int argc, char** argv) {
    CHECK(argc == 2);
    zb::Process process;
    bool called = false;
    process.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFE10) return false;
        CHECK(thread.regs()[0] == 7);
        thread.regs()[0] = 49;
        called = true;
        return true;
    });
    CHECK(process.run(argv[1], {argv[1]}, {}) == 0);
    CHECK(called);
    std::puts("host_call_dispatch_test PASS");
    return 0;
}
