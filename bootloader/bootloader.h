#ifndef BOOTLOADER_H
#define BOOTLOADER_H
#include <stdint.h>

#include "../main-ode/flippy.h"
/* Branch hints the permuter found; they reproduce the original block layout. */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// Boot status block read by 0xB4; the core 1 task also sends it to the ESP32.
extern struct fd_boot_status fd_status;

struct dol_list_t {
    char magic[4];
    char name[8][0x28];
} __attribute__((aligned(4)));
extern struct dol_list_t dol_list;

// Formats into a buffer and writes it to USB CDC 0; the DI and update paths log through it.
void cdc_printf(const char *fmt, ...);

#endif
