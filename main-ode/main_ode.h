#pragma once
#include "pico/mutex.h"
#include "flippy.h"
/* Branch hints the permuter found; they reproduce the original block layout. */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// di.c
extern mutex_t bus_mutex;
extern volatile bool bus_released;
extern int (*cmd_handlers[256])(void);
void set_cmd_handler(int cmd, int (*handler)(void));
void bus_release(void);
void di_error(uint32_t status);
void di_receive(uint32_t len, uint32_t offset);
void di_init_handlers(void);

// diskio.c
int mount_fat(void);

// audio.c
void audio_fill(void);
int audio_poll(void);
void adpcm_reset(void);
void adpcm_decode(const uint8_t *src, uint32_t *dst, uint32_t len);

// Not yet decompiled (pending.c).
void aud_set_buf_state(uint8_t buf, uint8_t state);
void aud_reset_bufs(void);
int aud_read(uint32_t pos, uint32_t len, void *buf, int fd);
void qspi_init(void);
void di_worker(void);
void flush_task(void);
int cmd_request_error(void);

// Link to the ESP32 (QSPI on PIO1) and the receive ring it fills.
struct esp_rx_buf {
    uint8_t data[0x2d000];
    uint32_t base;
    uint32_t wr;
    uint32_t rd;
    uint8_t active;
    volatile uint8_t split;  // 1 while the first 0x8000 bytes belong to audio
};
extern struct esp_rx_buf esp_rx;
extern mutex_t link_mutex;
extern mutex_t flash_mutex;
void esp_drain(void);
int qspi_send(const void *src, uint32_t len, uint32_t cont);
void esp_dma_read(void *dst, uint32_t len, uint32_t *done, int async);
extern volatile int aud_enabled;

#include "pico/runtime.h"
// DI command handlers. Each TU registers its own through the preinit array (REGISTER_CMD);
// GCC emits the entries last-defined first, so a TU declares them in reverse.
extern int bypass_enter(void);
extern int cmd_motor_stop(void);
extern int cmd_inquire(void);
extern int cmd_seek(uint32_t a, uint32_t offset);
extern int cmd_read(uint32_t a, uint32_t offset, uint32_t len);
extern int cmd_wkf(void);
extern int cmd_ipc(uint32_t a, uint32_t b, uint32_t c);
extern int cmd_audio_stream(uint32_t a, uint32_t b, uint32_t c);
extern int cmd_audio_status(uint32_t a, uint32_t b, uint32_t c);
extern int cmd_audio_config(uint32_t a, uint32_t b, uint32_t c);

#define REGISTER_CMD(cmd, fn)                                              static void register_##fn(void)                                        {                                                                          set_cmd_handler(cmd, (int (*)(void))fn);                           }                                                                      PICO_RUNTIME_INIT_FUNC_RUNTIME(register_##fn, "99999")

