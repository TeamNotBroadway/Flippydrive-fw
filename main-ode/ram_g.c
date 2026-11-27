// RAM-resident DI read over the ESP link: direct transfer and the readahead cache. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "main_ode.h"
#include "log.h"

#include "flippy.h"
#define REQ_MAGIC ESP_REQ_MAGIC

extern uint8_t root_dir_res;
extern int mount_h1;
extern volatile int aud_playing;
extern void esp_dma_wait(void);

// Byte after esp_rx.active: set while the first 0x8000 bytes of the ring belong to audio.
// Direct transfers use the part of the ring after the audio PCM buffers (0x2002b058).
#define net_buf (esp_rx.data + 0x23ee0)

struct __attribute__((packed)) net_req {
    uint32_t magic;
    uint8_t cmd, arg;
    uint8_t pad[2];
    uint32_t addr, len, rest[5];
};

static inline uint32_t net_imm_max(void)
{
    uint32_t whole = esp_rx.split == 0;
    return whole * 0x4000 + 0x12800;
}

static uint32_t net_last_addr, net_last_len;
uint32_t net_readahead = 2;

void __not_in_flash_func(flush_task)(void)
{
    mutex_enter_blocking(&link_mutex);
    mutex_enter_blocking(&flash_mutex);
    if (esp_rx.active) {
        uint32_t rd = esp_rx.rd;
        uint32_t wr = esp_rx.wr;
        uint32_t len = wr - rd;
        if (len) {
            uint32_t done;
            uint32_t base = esp_rx.base;
            done = 0;
            esp_dma_read(esp_rx.data + (rd - base), len, &done, 0);
            esp_rx.rd += done;
        }
    }
    mutex_exit(&flash_mutex);
    mutex_exit(&link_mutex);
}

int __not_in_flash_func(di_stream_net)(uint32_t addr, uint32_t len, uint sm, uint32_t fd)
{
    struct net_req r;
    dma_channel_config c;
    uint32_t ch;
    int mult;
    uint32_t done;

    if (mount_h1 != fd || root_dir_res != 0 || aud_playing != 0 || aud_enabled != 0 ||
        net_imm_max() < len)
        goto direct;

    if ((unlikely(!(addr == net_last_addr + net_last_len)))) {
        mult = 2;
    } else {
        uint32_t cur = net_readahead;
        mult = 2 + cur;
        int fit = 0x8000 / len;
        if (fit < mult)
            mult = fit;
        if (mult < 2)
            mult = 2;
    }
    net_readahead = mult;

    ch = dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(ch);
    channel_config_set_dreq(&c, pio_get_dreq(pio0, sm, true));
    dma_channel_configure(ch, &c, &pio0->txf[sm], NULL, 0, false);
    mutex_enter_blocking(&link_mutex);
    if (likely(aud_playing != 0 || aud_enabled != 0)) {
        if (unlikely(!esp_rx.split)) {
            LOG("FILE", "Splitting cache area\n");
            esp_rx.split = 1;
            goto refill;
        }
    }
    if (esp_rx.split != 0 && aud_playing == 0 && aud_enabled == 0) {
        LOG("FILE", "Joining cache area\n");
        esp_rx.split = 0;
        goto refill;
    }
    if (likely(!(esp_rx.active && esp_rx.base <= addr && addr <= esp_rx.wr - 3))) {
refill:
        mutex_enter_blocking(&flash_mutex);
        esp_drain();
        uint32_t whole = 0 == esp_rx.split;
        int32_t max = whole * 0x8000 + 0x25000;
        uint32_t total = len * mult;
        if (total > max)
            total = max;
        if (unlikely(total < 0x400))
            total = 0x400;
        esp_rx.base = addr;
        esp_rx.wr = addr + total;
        esp_rx.rd = addr;
        esp_rx.active = 1;
        memset((uint8_t *)&r + 6, 0, 26);
        r.magic = REQ_MAGIC; r.cmd = IPC_READ; r.arg = mount_h1; r.addr = addr; r.len = total;
        qspi_send(&r, 32, total);
        mutex_exit(&flash_mutex);
    }
    int32_t left = len;

    uint32_t pos = addr;
    mutex_exit(&link_mutex);
    while (left) {
        int32_t off;
        for (;;) {
            mutex_enter_blocking(&link_mutex);
            off = pos - esp_rx.base;
            if (likely(off < esp_rx.rd - esp_rx.base))
                break;
            mutex_enter_blocking(&flash_mutex);
            if (unlikely(esp_rx.active)) {
                uint32_t rd = esp_rx.rd;
                if (unlikely(rd != esp_rx.wr)) {
                    done = 0;
                    esp_dma_read(esp_rx.data + (rd - esp_rx.base), esp_rx.wr - rd, &done, 0);
                    esp_rx.rd += done;
                }
            }
            mutex_exit(&flash_mutex);
            mutex_exit(&link_mutex);
        }
        uint32_t span = esp_rx.wr - pos;
        if (left < span)
            span = left;
        mutex_exit(&link_mutex);
        while (span) {
            unsigned n;
            do {
                for (;;) {
                    mutex_enter_blocking(&link_mutex);
                    if (pos < esp_rx.rd)
                        break;
                    mutex_enter_blocking(&flash_mutex);
                    if (likely(esp_rx.active != 0)) {
                        uint32_t rd = esp_rx.rd;
                        if (unlikely(rd != esp_rx.wr)) {
                            done = 0;
                            esp_dma_read(esp_rx.data + (rd - esp_rx.base), esp_rx.wr - rd, &done, 0);
                            esp_rx.rd += done;
                        }
                    }
                    mutex_exit(&flash_mutex);
                    mutex_exit(&link_mutex);
                }
                n = esp_rx.rd - pos;
                if (left < n)
                    n = left;
                mutex_exit(&link_mutex);
            } while (!n);
            dma_channel_wait_for_finish_blocking(ch);
            dma_channel_set_read_addr(ch, esp_rx.data + off, false);
            span -= n;
            dma_channel_set_trans_count(ch, n >> 2, true);
            off += n;
            left -= n;
            pos += n;
        }
        dma_channel_wait_for_finish_blocking(ch);
        if (0 == left)
            break;
        mutex_enter_blocking(&link_mutex);
        mutex_enter_blocking(&flash_mutex);
        esp_drain();
        uint32_t whole = 0 == esp_rx.split;
        unsigned total = mult * left;
        uint32_t max = whole * 0x8000 + 0x25000;
        if (total > max)
            total = max;
        if (total < 0x400)
            total = 0x400;
        esp_rx.base = pos;
        esp_rx.wr = pos + total;
        esp_rx.rd = pos;
        esp_rx.active = 1;
        memset((uint8_t *)&r + 6, 0, 26);
        r.magic = REQ_MAGIC; r.cmd = IPC_READ; r.arg = mount_h1; r.addr = pos; r.len = total;
        qspi_send(&r, 32, total);
        mutex_exit(&flash_mutex);
        mutex_exit(&link_mutex);
    }
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
    net_last_addr = addr;
    net_last_len = len;
    return 0;

direct:;
    ch = dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(ch);
    channel_config_set_dreq(&c, pio_get_dreq(pio0, sm, true));
    dma_channel_configure(ch, &c, &pio0->txf[sm], NULL, 0, false);
    memset((uint8_t *)&r + 6, 0, 26);
    r.magic = REQ_MAGIC; r.cmd = IPC_READ; r.arg = fd; r.addr = addr; r.len = len;
    mutex_enter_blocking(&link_mutex);
    mutex_enter_blocking(&flash_mutex);
    if (esp_rx.rd != esp_rx.wr) {
        esp_rx.wr = esp_rx.rd;
        esp_drain();
    }
    esp_rx.active = 0;
    mutex_exit(&link_mutex);
    qspi_send(&r, 32, len);
    LOG("FILE", "Imm request data\n");
    esp_dma_read(net_buf, len, &done, 1);
    int buf = 0;
    if (len != 0) {
        do {
            esp_dma_wait();
            dma_channel_set_read_addr(ch, net_buf + buf * 0x4000, false);
            buf ^= 1;
            len -= done;
            dma_channel_set_trans_count(ch, done >> 2, true);
            if (0 != len)
                esp_dma_read(net_buf + buf * 0x4000, len, &done, 1);
            dma_channel_wait_for_finish_blocking((int)ch);
        } while (len);
    }
    mutex_exit(&flash_mutex);
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
    LOG("FILE", "Transfer complete - Imm\n");
    return 0;
}
