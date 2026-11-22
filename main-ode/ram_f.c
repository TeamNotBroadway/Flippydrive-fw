// RAM-resident audio DMA IRQ handler, AUDIO_SRC_CONFIG command, bitmap helpers and DI PIO setup. Names are ours.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "log.h"
#include "main_ode.h"

#define AUD_BUFS 9
#define AUD_CHUNK 0x1240
#define AUD_BUF_WORDS 0xff8
#define aud_pcm (*(uint32_t (*)[AUD_BUFS][AUD_BUF_WORDS])esp_rx.data)

extern volatile uint8_t aud_buf_state[AUD_BUFS];
extern volatile int aud_next_buf;
extern uint32_t aud_pos, aud_end, aud_cur_start, aud_cur_len, aud_loop_start, aud_last_len, aud_next_start, aud_next_len;
extern mutex_t aud_mutex;
extern uint ai_offset;
extern uint di_sm, di_offset;
void audio_dma_start(int buf);
void audio_dma_init(void);
void di_ack_error(void);
extern volatile int aud_playing, aud_loop;
extern volatile int aud_enabled;
extern uint ai_sm;
extern int aud_dma_a, aud_dma_b;
void audio_stop(void);

static inline void aud_dma_arm(uint ch, uint chain, int buf, uint32_t count)
{
    dma_channel_config c = dma_get_channel_config(ch);
    channel_config_set_chain_to(&c, chain);
    dma_channel_configure(ch, &c, &pio1->txf[ai_sm], aud_pcm[buf], count, false);
}

// Runs from the DMA IRQ with the mask of channels that finished: advance to the next PCM buffer.
void __not_in_flash_func(aud_dma_irq)(uint32_t status)
{
    uint8_t st = aud_buf_state[aud_next_buf];
    uint32_t more = aud_loop | aud_next_start;
    if (st == 2) {
        if (more == 0)
            aud_pos = aud_loop_start;
        aud_set_buf_state(aud_next_buf, 0);
        aud_enabled = 0;
        audio_stop();
        return;
    }
    uint32_t pos = aud_pos + AUD_CHUNK;
    aud_pos = pos;
    if (more == 0 && pos >= aud_end)
        aud_pos = aud_loop_start;
    aud_set_buf_state(aud_next_buf, 0);
    aud_next_buf++;
    aud_next_buf = aud_next_buf % AUD_BUFS;
    int nb = (uint)(aud_next_buf + 1) % AUD_BUFS;
    if (aud_buf_state[nb] == 1) {
        uint x = aud_dma_b, y = aud_dma_a;
        if (status & (1u << x))
            aud_dma_arm(x, y, nb, AUD_BUF_WORDS);
        if (status & (1u << y))
            aud_dma_arm(y, x, nb, AUD_BUF_WORDS);
    } else if (aud_buf_state[nb] == 2) {
        if (status & (1u << aud_dma_b))
            aud_dma_arm(aud_dma_b, aud_dma_b, nb, aud_last_len);
        if (status & (1u << aud_dma_a))
            aud_dma_arm(aud_dma_a, aud_dma_a, nb, aud_last_len);
    } else if (aud_playing) {
        (void)aud_enabled;
    }
}

static inline pio_sm_config ai_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 3, offset + 9);
    return c;
}

// DI command 0xe4: bits 17:16 of a select the operation, b and c give the start and length of the
// track in 32 KiB units.
int __not_in_flash_func(cmd_audio_stream)(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t op = (a & 0x3ffff) >> 16;
    if (op - 2 < 2) {
        di_error(DI_ERR_BAD_HANDLE);
        return 1;
    }
    uint32_t start = ((b & 0x3fffffff) >> 13) * 0x8000;
    uint32_t len = (c >> 15) * 0x8000;
    mutex_enter_blocking(&aud_mutex);
    if (op == 1) {
        aud_playing = 0;
        aud_enabled = 0;
        aud_loop = 0;
        audio_stop();
        LOG("DI", "AUDIO_SRC_CONFIG cancel playback\n");
    } else if (len == 0) {
        aud_loop = 0;
        LOG("DI", "AUDIO_SRC_CONFIG disable loop\n");
    } else if (aud_playing == 0) {
        if (aud_enabled != 0 && aud_loop == 0) {
            mutex_exit(&aud_mutex);
            di_error(0x52401);
            return 1;
        }
        aud_loop_start = start;
        aud_cur_start = start;
        aud_pos = start;
        aud_cur_len = len;
        aud_end = start + len;
        aud_loop = 1;
        aud_playing = 1;
        adpcm_reset();
        LOG("DI", "AUDIO_SRC_CONFIG enable\n");
        audio_stop();
        if (aud_playing)
            audio_fill();
        uint sm = ai_sm;
        uint offset = ai_offset;
        pio_sm_config cfg = ai_program_get_default_config(offset);
        sm_config_set_out_pins(&cfg, 29, 1);
        sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
        gpio_set_function(29, GPIO_FUNC_PIO1);
        pio_sm_set_consecutive_pindirs(pio1, sm, 29, 1, true);
        sm_config_set_out_shift(&cfg, false, false, 32);
        pio_sm_init(pio1, sm, offset, &cfg);
        pio_sm_exec(pio1, sm, 0xe020);
        pio_sm_set_enabled(pio1, sm, true);
        audio_dma_start(0);
        aud_enabled = 1;
    } else {
        aud_next_start = start;
        aud_next_len = len;
        aud_loop = 0;
        LOG("DI", "AUDIO_SRC_CONFIG set next track\n");
    }
    mutex_exit(&aud_mutex);
    pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "AUDIO_SRC_CONFIG OP:%d 0x%08X len 0x%X\n", op, start, len);
    di_ack_error();
    return 0;
}

// Per-index bit table; the flag byte disables lookups (bit 0 of the table is treated as set).
extern uint8_t id_bitmap[];
extern uint8_t ipc_cfg_dirty;

void __not_in_flash_func(id_bitmap_set)(uint32_t idx, int value)
{
    uint32_t i = idx >> 3;
    uint8_t v = id_bitmap[i];
    int bit = 1 << (idx & 7);
    if (value != 0)
        v |= bit;
    else
        v &= ~bit;
    id_bitmap[i] = v;
}

bool __not_in_flash_func(id_bitmap_test)(uint32_t idx)
{
    if (ipc_cfg_dirty != 0)
        return false;
    int v = id_bitmap[idx >> 3];
    unsigned bit = 1 << (idx & 7);
    if (idx == 0)
        return true;
    return (bit & v) != 0;
}

// Calls the handler registered for the command byte; 1 when there is none.
int di_dispatch(uint32_t a, uint32_t b, uint32_t c)
{
    int (*handler)(void) = cmd_handlers[a >> 24];
    if (handler == NULL)
        return 1;
    ((int (*)(uint32_t, uint32_t, uint32_t))handler)(a, b, c);
    return 0;
}

#define DI_PIN_DSTRB 28
#define DI_PIN_BRK 21
#define DI_PIN_RESET 26

// Three-instruction program on two state machines that drive the data pins' direction
// (sideset pindirs on GPIO 0-3 and 4-7).
static const uint16_t di_dir_instructions[] = {0x3e9a, 0x209b, 0x3e1b};
static const struct pio_program di_dir_program = {di_dir_instructions, 3, -1};

// The DI bus state machine; words and bytes of the commands go through it.
static const uint16_t di_instructions[] = {
    0x309a, 0x301b, 0x2019, 0x2099, 0x4008, 0x0042, 0x3019, 0x3099, 0x5008, 0x1086, 0x309b,
    0x3019, 0x6308, 0xf321, 0x6308, 0xb142, 0x10d2, 0x100c, 0xb042, 0x6308, 0x1353, 0x100b,
};
static const struct pio_program di_program = {di_instructions, 22, -1};

uint di_dir_sm_lo, di_dir_sm_hi;

static inline pio_sm_config di_dir_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 1, offset + 2);
    sm_config_set_sideset(&c, 4, false, true);
    return c;
}

static inline pio_sm_config di_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 21);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

void di_gpio_irq(uint gpio, uint32_t events);

void di_setup(void)
{
    uint dir_offset = pio_add_program(pio0, &di_dir_program);
    di_dir_sm_lo = pio_claim_unused_sm(pio0, true);
    di_dir_sm_hi = pio_claim_unused_sm(pio0, true);
    di_offset = pio_add_program(pio0, &di_program);
    di_sm = pio_claim_unused_sm(pio0, true);
    LOG("DMA", "Configuring Audio DMA\n");
    aud_reset_bufs();
    audio_dma_init();

    uint sm = di_dir_sm_lo;
    pio_sm_config c = di_dir_program_get_default_config(dir_offset);
    pio_sm_set_consecutive_pindirs(pio0, sm, 0, 4, false);
    sm_config_set_sideset_pins(&c, 0);
    pio_sm_init(pio0, sm, dir_offset, &c);
    pio_sm_set_enabled(pio0, sm, true);

    sm = di_dir_sm_hi;
    c = di_dir_program_get_default_config(dir_offset);
    pio_sm_set_consecutive_pindirs(pio0, sm, 4, 4, false);
    sm_config_set_sideset_pins(&c, 4);
    pio_sm_init(pio0, sm, dir_offset, &c);
    pio_sm_set_enabled(pio0, sm, true);

    uint offset = di_offset;
    sm = di_sm;
    c = di_program_get_default_config(offset);
    sm_config_set_out_pins(&c, 0, 8);
    pio_sm_set_consecutive_pindirs(pio0, sm, DI_PIN_DSTRB, 1, true);
    sm_config_set_sideset_pins(&c, DI_PIN_DSTRB);
    sm_config_set_jmp_pin(&c, 25);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_out_shift(&c, true, true, 32);
    pio_sm_init(pio0, sm, offset, &c);
    pio_sm_exec(pio0, sm, pio_encode_set(pio_x, 8) | pio_encode_sideset(1, 1));
    pio_sm_exec(pio0, sm, pio_encode_set(pio_y, 2) | pio_encode_sideset(1, 1));
    gpio_set_function(DI_PIN_DSTRB, GPIO_FUNC_PIO0);
    pio_sm_set_enabled(pio0, sm, true);

    if (gpio_get(DI_PIN_BRK))
        di_gpio_irq(DI_PIN_BRK, GPIO_IRQ_EDGE_RISE);
    gpio_set_irq_enabled(DI_PIN_BRK, GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}
