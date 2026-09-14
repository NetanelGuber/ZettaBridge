#pragma once

#include <stdint.h>

#define ZB_SERVICE_PROTOCOL_VERSION 1u
#define ZB_SERVICE_READY_INDEX 0xFE00u
#define ZB_CARRIER_PARK_INDEX 0xFE01u
#define ZB_SERVICE_SCRATCH_SIZE 4096u

struct zb_service_api {
    uint32_t size;
    uint32_t version;
    uint32_t dlopen_fn;
    uint32_t dlsym_fn;
    uint32_t dlerror_fn;
    uint32_t spawn_carrier_fn;
    uint32_t scratch;
    uint32_t scratch_size;
};
