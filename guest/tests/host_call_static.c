#include <stdint.h>

int main(void) {
    register uint32_t r0 __asm__("r0") = 7;
    __asm__ volatile("svc #0x5afe10" : "+r"(r0) : : "memory");
    return r0 == 49 ? 0 : 1;
}
