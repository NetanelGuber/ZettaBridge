#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "zb/library_protocol.h"

static char scratch[ZB_SERVICE_SCRATCH_SIZE];

static uint32_t host_call(uint32_t index, uint32_t arg) {
    register uint32_t r0 __asm__("r0") = arg;
    if (index == ZB_SERVICE_READY_INDEX) {
        __asm__ volatile("svc #0x5afe00" : "+r"(r0) : : "memory");
    } else {
        __asm__ volatile("svc #0x5afe01" : "+r"(r0) : : "memory");
    }
    return r0;
}

static uint32_t service_dlopen(const char* path, uint32_t flags) {
    return (uint32_t)(uintptr_t)dlopen(path, (int)flags);
}

static uint32_t service_dlsym(uint32_t handle, const char* name) {
    return (uint32_t)(uintptr_t)dlsym((void*)(uintptr_t)handle, name);
}

static uint32_t service_dlerror(void) {
    return (uint32_t)(uintptr_t)dlerror();
}

static void* carrier_main(void* unused) {
    (void)unused;
    host_call(ZB_CARRIER_PARK_INDEX, 0);
    return NULL;
}

static uint32_t spawn_carrier(void) {
    pthread_t thread;
    const int rc = pthread_create(&thread, NULL, carrier_main, NULL);
    if (rc == 0) pthread_detach(thread);
    return (uint32_t)rc;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* dl_android = dlopen("libdl_android.so", RTLD_NOW);
    void (*set_target_sdk)(unsigned) = dl_android != NULL
        ? (void (*)(unsigned))dlsym(dl_android, "android_set_application_target_sdk_version")
        : NULL;
    if (set_target_sdk == NULL) return 3;
    set_target_sdk((unsigned)strtoul(argv[1], NULL, 10));
    (void)dlopen("libzbcompat.so", RTLD_NOW | RTLD_GLOBAL);
    const struct zb_service_api api = {
        sizeof(api), ZB_SERVICE_PROTOCOL_VERSION,
        (uint32_t)(uintptr_t)service_dlopen,
        (uint32_t)(uintptr_t)service_dlsym,
        (uint32_t)(uintptr_t)service_dlerror,
        (uint32_t)(uintptr_t)spawn_carrier,
        (uint32_t)(uintptr_t)scratch, sizeof(scratch),
    };
    return (int)host_call(ZB_SERVICE_READY_INDEX, (uint32_t)(uintptr_t)&api);
}
