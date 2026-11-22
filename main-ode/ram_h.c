// RAM-resident DI audio commands: buffer config (0xE4) and audio status (0xE2). Names are ours.
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "main_ode.h"
#include "log.h"

extern uint di_sm;
extern uint32_t aud_pos, aud_cur_start, aud_cur_len, aud_loop_start;
extern void di_ack_error(void);

int __not_in_flash_func(cmd_audio_config)(uint32_t a, uint32_t b, uint32_t c)
{
    sleep_ms(1);
    pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "AUDIO_BUF_CONFIG 0x%08X\n", a);
    di_ack_error();
    return 0;
}

// a bits 17:16 select the field reported: 0 playing flag, 1 position, 2 start address, 3 length.
int __not_in_flash_func(cmd_audio_status)(uint32_t a, uint32_t b, uint32_t c)
{
    switch ((a & 0x3ffff) >> 16) {
    default: {
        uint32_t en = aud_enabled != 0;
        pio_sm_put_blocking(pio0, di_sm, en << 24);
        LOG("DI", "AUDIO_STATUS STATUS 0x%08X\n", en);
        break;
    }
    case 3:
        pio_sm_put_blocking(pio0, di_sm, __builtin_bswap32(aud_cur_len));
        LOG("DI", "AUDIO_STATUS LEN 0x%08X\n", aud_cur_len);
        break;
    case 1:
        pio_sm_put_blocking(pio0, di_sm, ((aud_pos >> 10) & 0xff00) | (aud_pos >> 26) | ((aud_pos << 6) & 0xe00000));
        LOG("DI", "AUDIO_STATUS CUR_ADDR 0x%08X (read head 0x%08X)\n", (aud_pos >> 15) << 15, aud_cur_start);
        break;
    case 2: {
        uint32_t w = aud_loop_start >> 2;
        pio_sm_put_blocking(pio0, di_sm, (w << 24) | ((aud_loop_start >> 10) & 0xff00) | (aud_loop_start >> 26) | ((w << 8) & 0xff0000));
        LOG("DI", "AUDIO_STATUS START ADDR 0x%08X\n", aud_loop_start);
        break;
    }
    }
    di_ack_error();
    return 0;
}
