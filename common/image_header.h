#pragma once
#include <stdint.h>

// Header the bootloader looks for in each image. The markers are pico-sdk's binary_info
// markers with their 16-bit halves swapped; the ids are binary_info's program-name and
// version-string ids.
#define IMAGE_HEADER_MARKER_START 0xebf27188u
#define IMAGE_HEADER_MARKER_END   0xa390e71au

typedef struct {
    uint32_t marker_start;
    uint32_t name_id;
    char name[24];
    uint32_t version_id;
    char version[12];
    uint32_t marker_end;
} image_header_t;

#define IMAGE_HEADER(prog_name, prog_version)                                      \
    __attribute__((section(".image_header"), used, aligned(16)))                  \
    const image_header_t image_header = {                                          \
        IMAGE_HEADER_MARKER_START, 0x02031c86u, prog_name,                         \
        0x11a9bc3au, prog_version, IMAGE_HEADER_MARKER_END}
