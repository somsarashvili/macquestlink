#pragma once
#include <stdint.h>

typedef struct encoder_output {
    uint8_t *data;
    uint32_t size;
    uint32_t keyframe;
} encoder_output;

void trace(const char *fmt, ...);

extern volatile long g_stage;
extern double g_stage_ms[4];

int encoder_client_encode(void *texture, uint32_t flags, uint64_t timestamp, encoder_output *out);
void encoder_client_register(void *texture);
void encoder_client_reset(void);
void encoder_client_set_rate(uint32_t avg_bps, uint32_t max_bps, uint32_t vbv_bits, uint32_t fps);
void encoder_client_set_slices(uint32_t slices);
