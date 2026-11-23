// Placeholders for functions main calls that are not decompiled yet, so the image links.
// They are not the original's behaviour (docs/HACKS.md).
#include <stdlib.h>
#include "pico/stdlib.h"
#include "main_ode.h"
#include "flippy.h"

#include "ff.h"
#include "hardware/pio.h"

// Raw instructions from the original's struct at 0x10093d10; not decoded to .pio source.
static const uint16_t ai_instr[10] = {0x2a96, 0x2a16, 0x2096, 0x8080, 0xe05f, 0x2a96, 0x2018, 0x6001, 0x2a98, 0x0086};
const struct pio_program ai_program = {.instructions = ai_instr, .length = 10, .origin = -1};
uint ai_offset;
DIR root_dir;
FIL di_file;
int mount_h2, mount_h1;
uint32_t esp_dma_ch;
volatile uint32_t esp_remaining;
bool esp_booted;
uint8_t net_stream_active;
uint8_t ipc_cfg_dirty;
uint8_t ipc_open_ro;
int ini_result;
uint64_t di_file_size;
struct ipc_cfg_t ipc_cfg;
uint8_t id_bitmap[32];
