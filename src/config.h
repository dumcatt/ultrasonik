#pragma once

#include <stdint.h>

struct config {
    char device[256];       /* ASIO device name, empty = first available */
    uint32_t sample_rate;   /* Default 44100 */
    uint32_t bit_depth;     /* Default 24 (16, 24, or 32) */
    uint32_t buffer_size;   /* Default 192 (~4ms at 44100Hz). 0 = driver preferred */
};

void config_load(struct config *cfg);
