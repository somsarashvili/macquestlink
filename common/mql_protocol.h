#pragma once
#include <stdint.h>

#define MQL_PORT 47800
#define MQL_FRAME_FILE_WIN "C:\\macquestlink\\frames.bin"
#define MQL_SLOT_SIZE (64u << 20)
#define MQL_SLOT_COUNT 2u
#define MQL_MAX_BITSTREAM (8u << 20)

#define MQL_MAGIC_FRAME 0x534c514du
#define MQL_MAGIC_OUTPUT 0x4f4c514du

#define MQL_FLAG_FORCE_IDR 1u
#define MQL_FLAG_PARAMETER_SETS 2u

#pragma pack(push, 1)
typedef struct mql_frame_request {
    uint32_t magic;
    uint32_t slot;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t chroma_offset;
    uint32_t ten_bit;
    uint32_t flags;
    uint32_t bitrate_kbps;
    uint32_t fps;
    uint64_t timestamp;
    uint32_t max_bitrate_kbps;
    uint32_t vbv_bits;
    uint32_t slices;
    uint32_t surface; // reserved, 0
} mql_frame_request;

typedef struct mql_frame_reply {
    uint32_t magic;
    int32_t status;
    uint32_t keyframe;
    uint32_t size;
} mql_frame_reply;
#pragma pack(pop)
