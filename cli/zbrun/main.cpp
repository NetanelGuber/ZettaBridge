#include <cstdio>

#include <dynarmic/interface/exclusive_monitor.h>

int main() {
    Dynarmic::ExclusiveMonitor monitor(1);
    std::puts("zbrun ok");
    return 0;
}
