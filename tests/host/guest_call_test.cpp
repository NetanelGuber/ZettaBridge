#include <sys/mman.h>

#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/native_call.h"

namespace {

void put32(zb::GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    std::memcpy(mem.base() + addr, &value, sizeof value);
}

}  // namespace

int main() {
    zb::GuestMemory mem;
    CHECK(mem.ok());
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x20000, 0x2000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0xFFFF0000, 0x1000, PROT_READ | PROT_WRITE));

    put32(mem, 0x10000, 0xE0800001);  // add r0, r0, r1
    put32(mem, 0x10004, 0xE59D2000);  // ldr r2, [sp]
    put32(mem, 0x10008, 0xE0800002);  // add r0, r0, r2
    put32(mem, 0x1000C, 0xEF5A0010);  // svc #0x5a0010
    put32(mem, 0x10010, 0xE12FFF1E);  // bx lr
    put32(mem, zb::kHostReturnAddress, 0xEF5AFFFF);  // svc #0x5affff
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    CHECK(mem.protect(0xFFFF0000, 0x1000, PROT_READ | PROT_EXEC));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(mem, &monitor, 0);
    thread.regs().fill(0xA5A5A5A5);
    thread.ext_regs().fill(0x5A5A5A5A);
    thread.regs()[13] = 0x22000;
    thread.regs()[15] = 0x12345678;
    thread.set_cpsr(0x10);
    thread.set_fpscr(0x01000000);
    const auto saved_regs = thread.regs();
    const auto saved_ext = thread.ext_regs();

    zb::GuestCall call;
    call.regs = {10, 20, 0, 0};
    call.stack = {12};
    int host_calls = 0;
    const auto result = thread.call(0x10000, call, [&](const zb::Stop& stop) {
        CHECK(stop.kind == zb::StopKind::Svc && stop.swi == 0x5A0010);
        ++host_calls;
        thread.regs()[0] += 0;  // handler may update call registers
        return true;
    });

    CHECK(result && result->r0 == 42 && result->r1 == 20);
    CHECK(host_calls == 1);
    CHECK(thread.regs() == saved_regs);
    CHECK(thread.ext_regs() == saved_ext);
    CHECK(thread.cpsr() == 0x10 && thread.fpscr() == 0x01000000);

    zb::GuestCall too_large;
    too_large.stack.resize(0x1000);
    thread.regs()[13] = 0x20004;
    CHECK(!thread.call(0x10000, too_large, [](const zb::Stop&) { return false; }));

    std::puts("guest_call_test PASS");
    return 0;
}
