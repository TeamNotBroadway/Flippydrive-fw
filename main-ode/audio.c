// DVD audio streaming: ADPCM read from the disc image in 0x1240-byte chunks, decoded into
// AUD_BUFS PCM buffers, played to AISD by PIO1's ai_program through two chained DMA
// channels. Names are ours; the log strings are the firmware's.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "log.h"
#include "main_ode.h"

#define AUD_BUFS 9
#define AUD_CHUNK 0x1240
#define AUD_BUF_WORDS 0xff8

// The PCM buffers are the start of the ESP receive ring (both 0x20007178 in the original).
#define aud_pcm (*(uint32_t (*)[AUD_BUFS][AUD_BUF_WORDS])esp_rx.data)
uint8_t aud_file_buf[AUD_CHUNK];
volatile uint8_t aud_buf_state[AUD_BUFS];
volatile int aud_next_buf;
int aud_fd;
uint32_t aud_pos;
uint32_t aud_end;
uint32_t aud_cur_start;
uint32_t aud_cur_len;
uint32_t aud_loop_start;
uint32_t aud_last_len;
uint32_t aud_next_start;
uint32_t aud_next_len;
volatile int aud_playing;
volatile int aud_loop;
uint ai_sm;
int aud_dma_a;
int aud_dma_b;
critical_section_t aud_cs;

// Fill every free PCM buffer, starting at aud_next_buf.
void audio_fill(void)
{
    int start = aud_next_buf;
    for (int i = start; i != start + AUD_BUFS; i++) {
        int b = i % AUD_BUFS;
        if (aud_buf_state[b] != 0)
            continue;
        uint32_t pos = aud_pos;
        uint32_t end = aud_end;
        uint32_t len, rest;
        if (pos + AUD_CHUNK < end) {
            len = AUD_CHUNK;
            rest = 0;
        } else {
            len = end - pos;
            rest = pos + AUD_CHUNK - end;
        }
        LOG("AUD", "reading from 0x%08X len 0x%08X into buf %d\n", aud_pos, len, b);
        aud_read(aud_pos, len, aud_file_buf, aud_fd);
        adpcm_decode(aud_file_buf, aud_pcm[b], len);
        aud_pos += len;
        if (pos + AUD_CHUNK >= end) {
            if (!aud_loop) {
                if (aud_next_start == 0) {
                    memset(&aud_pcm[b][len], 0, rest);
                    aud_last_len = len;
                    aud_set_buf_state(b, 2);
                    LOG("AUD", "track terminating at 0x%08X\n", aud_pos);
                    aud_playing = 0;
                    return;
                }
                uint32_t next = aud_next_start, next_len = aud_next_len;
                aud_loop_start = next;
                aud_cur_start = next;
                aud_cur_len = next_len;
                aud_pos = next;
                aud_end = next + next_len;
                aud_next_start = 0;
                aud_next_len = 0;
                adpcm_reset();
                aud_read(aud_pos, rest, aud_file_buf, aud_fd);
                LOG("AUD", "NEXT read from 0x%08X len 0x%08X\n", aud_pos, rest);
                adpcm_decode(aud_file_buf, &aud_pcm[b][len * 7 / 8], rest);
                aud_set_buf_state(b, 1);
                LOG("AUD", "track playing next at 0x%08X\n", aud_pos);
                aud_pos += rest;
            } else {
                aud_cur_start = aud_loop_start;
                aud_pos = aud_loop_start;
                adpcm_reset();
                aud_read(aud_pos, rest, aud_file_buf, aud_fd);
                LOG("AUD", "LOOP read from 0x%08X len 0x%08X\n", aud_pos, rest);
                adpcm_decode(aud_file_buf, &aud_pcm[b][len * 7 / 8], rest);
                aud_pos += rest;
                aud_set_buf_state(b, 1);
            }
        } else {
            aud_set_buf_state(b, 1);
        }
    }
}

void audio_stop(void)
{
    pio_sm_set_enabled(pio1, ai_sm, false);
    pio_sm_drain_tx_fifo(pio1, ai_sm);
    pio_sm_clear_fifos(pio1, ai_sm);
    pio_sm_restart(pio1, ai_sm);
    dma_channel_set_irq0_enabled(aud_dma_a, false);
    dma_channel_set_irq0_enabled(aud_dma_b, false);
    dma_channel_abort(aud_dma_a);
    dma_channel_abort(aud_dma_b);
    dma_channel_acknowledge_irq0(aud_dma_a);
    dma_channel_acknowledge_irq0(aud_dma_b);
    aud_next_buf = 0;
    aud_reset_bufs();
}

void audio_reset(void)
{
    aud_playing = 0;
    aud_enabled = 0;
    aud_loop = 0;
    audio_stop();
}

// Start playing buffer buf, with the next buffer queued on the other channel.
void audio_dma_start(int buf)
{
    uint32_t *next = (uint)(buf + 1) < AUD_BUFS ? aud_pcm[buf + 1] : aud_pcm[0];
    LOG("AUD", "enabling DMA\n");
    dma_channel_set_irq0_enabled(aud_dma_a, true);
    dma_channel_set_irq0_enabled(aud_dma_b, true);
    dma_channel_config c = dma_get_channel_config(aud_dma_b);
    channel_config_set_chain_to(&c, aud_dma_a);
    dma_channel_configure(aud_dma_b, &c, &pio1->txf[ai_sm], next, AUD_BUF_WORDS, false);
    c = dma_get_channel_config(aud_dma_a);
    channel_config_set_chain_to(&c, aud_dma_b);
    dma_channel_configure(aud_dma_a, &c, &pio1->txf[ai_sm], aud_pcm[buf], AUD_BUF_WORDS, true);
}

int audio_poll(void)
{
    if (!aud_playing)
        return 0;
    audio_fill();
    return 0;
}

REGISTER_CMD(DI_CMD_AUDIO_CONFIG, cmd_audio_config);
REGISTER_CMD(DI_CMD_AUDIO_STATUS, cmd_audio_status);
REGISTER_CMD(DI_CMD_AUDIO_STREAM, cmd_audio_stream);

void audio_dma_init(void)
{
    critical_section_init(&aud_cs);
    aud_dma_a = dma_claim_unused_channel(true);
    aud_dma_b = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(aud_dma_a);
    channel_config_set_dreq(&c, pio_get_dreq(pio1, ai_sm, true));
    channel_config_set_chain_to(&c, aud_dma_b);
    dma_channel_configure(aud_dma_a, &c, &pio1->txf[ai_sm], aud_pcm[0], AUD_BUF_WORDS, false);
    c = dma_channel_get_default_config(aud_dma_b);
    channel_config_set_dreq(&c, pio_get_dreq(pio1, ai_sm, true));
    dma_channel_configure(aud_dma_b, &c, &pio1->txf[ai_sm], aud_pcm[1], AUD_BUF_WORDS, false);
}

void audio_dma_deinit(void)
{
    critical_section_deinit(&aud_cs);
    dma_channel_unclaim(aud_dma_a);
    dma_channel_unclaim(aud_dma_b);
}
