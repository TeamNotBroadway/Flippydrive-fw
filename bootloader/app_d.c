// Bootloader DI command handlers, DI GPIO IRQ, core 1 entry, ESP QSPI transfers. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/busctrl.h"
#include "bootloader.h"

#define DI_PIN_DSTRB 28
#define DI_PIN_DIR 27
#define DI_PIN_BRK 21
#define DI_PIN_RESET 26

uint di_sm;
uint di_offset;
extern const struct pio_program di_program;
extern const struct pio_program di_dir_program;
extern const uint8_t gc_disc[];
extern const uint32_t gc_disc_size;
volatile uint8_t update_request;
volatile uint8_t boot_request;
volatile uint8_t esp_ready;
volatile uint8_t force_update;
volatile uint8_t factory_flag;
uint32_t di_pin_table;
extern uint qspi_in_sm, qspi_out_sm, qspi_out_status_sm;
extern uint qspi_in_offset, qspi_out_offset, qspi_out_status_offset;
uint32_t esp_dma_ch;
uint32_t esp_remaining;

void expander_set(uint pin, bool on);
void di_dir_sm_init(uint sm, uint offset, uint pin);
int di_dispatch(uint32_t a, uint32_t b, uint32_t c);
void bypass_pins_init(void);
void di_gpio_irq(uint gpio, uint32_t events);
void qspi_reset_sms(void);

int __not_in_flash_func(cmd_fd_status)(void)
{
    const uint32_t *p = (const uint32_t *)&fd_status;
    for (int i = 0; i < (int)(sizeof fd_status / 4); i++)
        pio_sm_put_blocking(pio0, di_sm, p[i]);
    return 0;
}

int __not_in_flash_func(cmd_request_error)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    cdc_printf("DI ERROR_STATUS\n");
    return 0;
}

int __not_in_flash_func(cmd_motor_stop)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    cdc_printf("DI MOTOR_STOP\n");
    return 0;
}

int __not_in_flash_func(cmd_read)(uint32_t a, uint32_t offset, uint32_t len)
{
    uint32_t pos = offset << 2;
    len &= ~31u;
    cdc_printf("DI READ 0x%08X len 0x%X\n", pos, len);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        if (pos + i < gc_disc_size)
            w = *(const uint32_t *)(gc_disc + pos + i);
        pio_sm_put_blocking(pio0, di_sm, w);
        if (!gpio_get(DI_PIN_RESET))
            break;
    }
    return 0;
}

int __not_in_flash_func(cmd_dol_list)(void)
{
    const uint32_t *p = (const uint32_t *)&dol_list;
    for (int i = 0; i < 88; i++)
        pio_sm_put_blocking(pio0, di_sm, p[i]);
    for (int i = 0; i < 8; i++)
        cdc_printf("DI FIRMWARE_DOL_LIST %s\n", dol_list.name[i]);
    cdc_printf("DI FIRMWARE_DOL_LIST COMPLETE\n");
    return 0;
}

int __not_in_flash_func(cmd_seek)(uint32_t a, uint32_t offset)
{
    cdc_printf("DI SEEK 0x%08X\n", offset << 2);
    pio_sm_put_blocking(pio0, di_sm, 0);
    (void)sio_hw->gpio_in;
    return 0;
}

// The magic values in c select which flag is raised; the reply is the fixed inquiry block.
int __not_in_flash_func(cmd_inquire)(uint32_t a, uint32_t b, uint32_t c)
{
    if (b == INQUIRY_BOOT_MAGIC) {
        switch (c) {
        case INQUIRY_UPDATE:
            update_request = 1;
            break;
        case INQUIRY_BOOT:
            expander_set(6, true);
            boot_request = 1;
            break;
        case INQUIRY_ESP_READY:
            expander_set(6, true);
            esp_ready = 1;
            break;
        }
    }
    pio_sm_put_blocking(pio0, di_sm, 1);
    pio_sm_put_blocking(pio0, di_sm, 0x20042220);
    pio_sm_put_blocking(pio0, di_sm, __builtin_bswap32((factory_flag ? 0x06000002 : 0x06000000) | force_update));
    pio_sm_put_blocking(pio0, di_sm, 0x0c000401);
    for (int i = 0; i < 4; i++)
        pio_sm_put_blocking(pio0, di_sm, 0);
    cdc_printf("DI INQUIRE 0x%08X 0x%08X\n", b, c);
    return 0;
}

int __not_in_flash_func(cmd_audio_config)(uint32_t a, uint32_t b, uint32_t c)
{
    sleep_ms(1);
    pio_sm_put_blocking(pio0, di_sm, 0);
    cdc_printf("DI STUB AUDIO_BUF_CONFIG 0x%08X\n", a);
    return 0;
}

static inline void di_sm_stop_inline(void)
{
    pio_sm_set_enabled(pio0, di_sm, false);
    gpio_put(DI_PIN_DSTRB, 1);
    gpio_set_dir(DI_PIN_DSTRB, GPIO_OUT);
    gpio_set_function(DI_PIN_DSTRB, GPIO_FUNC_SIO);
    pio_sm_drain_tx_fifo(pio0, di_sm);
    pio_sm_clear_fifos(pio0, di_sm);
    pio_sm_restart(pio0, di_sm);
}

static inline void di_sm_start(void)
{
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_set(pio_x, 8) | pio_encode_sideset(1, 1));
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_set(pio_y, 2) | pio_encode_sideset(1, 1));
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_jmp(di_offset) | pio_encode_sideset(1, 1));
    gpio_set_function(DI_PIN_DSTRB, GPIO_FUNC_PIO0);
    pio_sm_set_enabled(pio0, di_sm, true);
}

// DIR falling: restart the state machine. BRK rising: reset the bus. RESET level: restart.
void __not_in_flash_func(di_gpio_irq)(uint gpio, uint32_t events)
{
    if (gpio == DI_PIN_DIR && (events & GPIO_IRQ_EDGE_FALL)) {
        di_sm_stop_inline();
        di_sm_start();
        gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, false);
    }
    if (gpio == DI_PIN_BRK && (events & GPIO_IRQ_EDGE_RISE)) {
        di_sm_stop_inline();
        gpio_put(DI_PIN_BRK, 0);
        gpio_set_dir(DI_PIN_BRK, GPIO_OUT);
        cdc_printf("DI BRK received\n");
        busy_wait_ms(50);
        gpio_set_dir(DI_PIN_BRK, GPIO_IN);
        di_sm_start();
    }
    if (gpio == DI_PIN_RESET) {
        if (events & GPIO_IRQ_LEVEL_LOW) {
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW, false);
            di_sm_stop_inline();
            di_sm_start();
            cdc_printf("DI Reset drive\n");
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_HIGH, true);
        } else if (events & GPIO_IRQ_LEVEL_HIGH) {
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_HIGH, false);
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW, true);
        }
    }
    gpio_acknowledge_irq(gpio, events);
}

__attribute__((noinline)) void __not_in_flash_func(di_sm_stop)(void)
{
    di_sm_stop_inline();
}

// Hand the bus to the pass-through pin setup; does not return.
int __not_in_flash_func(bypass_enter)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    cdc_printf("DI Entering Bypass\n");
    gpio_set_irq_enabled(DI_PIN_BRK, GPIO_IRQ_EDGE_RISE, false);
    gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH, false);
    gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, false);
    irq_set_enabled(IO_IRQ_BANK0, false);
    di_sm_stop();
    bypass_pins_init();
    for (;;)
        ;
}

static inline pio_sm_config di_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 21);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

static inline void di_program_init(PIO pio, uint sm, uint offset)
{
    pio_sm_config c = di_program_get_default_config(offset);
    sm_config_set_out_pins(&c, 0, 8);
    sm_config_set_set_pins(&c, 0, 0);
    pio_sm_set_consecutive_pindirs(pio, sm, DI_PIN_DSTRB, 1, true);
    sm_config_set_sideset_pins(&c, DI_PIN_DSTRB);
    sm_config_set_jmp_pin(&c, 25);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_out_shift(&c, true, true, 32);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 8) | pio_encode_sideset(1, 1));
    pio_sm_exec(pio, sm, pio_encode_set(pio_y, 2) | pio_encode_sideset(1, 1));
    gpio_set_function(DI_PIN_DSTRB, GPIO_FUNC_PIO0);
    pio_sm_set_enabled(pio, sm, true);
}

void __not_in_flash_func(core1_entry)(void)
{
    flash_safe_execute_core_init();
    multicore_lockout_victim_init();
    cdc_printf("DI Starting up PIO DI\n");
    di_pin_table = 0x00010203;
    gpio_set_irq_callback(di_gpio_irq);
    irq_set_enabled(IO_IRQ_BANK0, true);
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    uint dir_off = pio_add_program(pio0, &di_dir_program);
    uint dir_sm_lo = pio_claim_unused_sm(pio0, true);
    uint dir_sm_hi = pio_claim_unused_sm(pio0, true);
    di_offset = pio_add_program(pio0, &di_program);
    di_sm = pio_claim_unused_sm(pio0, true);
    cdc_printf("DI dir_ctl_program 1 offset %d\n", dir_off);
    cdc_printf("DI dir_ctl_program 2 offset %d\n", dir_off);
    cdc_printf("DI di_program offset %d\n", di_offset);
    di_dir_sm_init(dir_sm_lo, dir_off, 0);
    di_dir_sm_init(dir_sm_hi, dir_off, 4);
    di_program_init(pio0, di_sm, di_offset);
    gpio_set_irq_enabled(DI_PIN_BRK, GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH, true);
    for (;;) {
        while (pio_sm_is_rx_fifo_empty(pio0, di_sm))
            ;
        uint32_t a = pio0->rxf[di_sm];
        while (pio_sm_is_rx_fifo_empty(pio0, di_sm))
            ;
        uint32_t b = pio0->rxf[di_sm];
        while (pio_sm_is_rx_fifo_empty(pio0, di_sm))
            ;
        uint32_t d = pio0->rxf[di_sm];
        gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, true);
        if (di_dispatch(a, b, d)) {
            pio_sm_put_blocking(pio0, di_sm, 0);
            cdc_printf("DI !!!UNHANDLED CMD %02X!!!\n", a >> 24);
        }
    }
}

// Stop, flush and restart the three QSPI state machines at their program starts.
void __not_in_flash_func(qspi_reset_sms)(void)
{
    pio_sm_set_enabled(pio1, qspi_out_sm, false);
    pio_sm_set_enabled(pio1, qspi_out_status_sm, false);
    pio_sm_set_enabled(pio1, qspi_in_sm, false);
    pio_sm_clear_fifos(pio1, qspi_out_sm);
    pio_sm_clear_fifos(pio1, qspi_out_status_sm);
    pio_sm_clear_fifos(pio1, qspi_in_sm);
    pio_sm_restart(pio1, qspi_out_sm);
    pio_sm_restart(pio1, qspi_out_status_sm);
    pio_sm_restart(pio1, qspi_in_sm);
    pio_sm_exec_wait_blocking(pio1, qspi_out_sm, pio_encode_jmp(qspi_out_offset));
    pio_sm_exec_wait_blocking(pio1, qspi_out_status_sm, pio_encode_jmp(qspi_out_status_offset));
    pio_sm_exec_wait_blocking(pio1, qspi_in_sm, pio_encode_jmp(qspi_in_offset));
}

// Receive len bytes (clamped to 0x4000) from the ESP over PIO1 RX into dst.
void __not_in_flash_func(esp_dma_read)(void *dst, uint32_t len, uint32_t *done, int async)
{
    int ch = dma_claim_unused_channel(true);
    esp_dma_ch = ch;
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio1, qspi_in_sm, false));
    channel_config_set_bswap(&c, true);
    channel_config_set_high_priority(&c, true);
    if (len > 0x4000)
        len = 0x4000;
    *done = len;
    dma_channel_configure(esp_dma_ch, &c, dst, &pio1->rxf[qspi_in_sm], len >> 2, true);
    gpio_put(20, 0);
    while (gpio_get(17))
        ;
    gpio_put(20, 1);
    esp_remaining -= len;
    if (likely(async))
        return;
    dma_channel_wait_for_finish_blocking(esp_dma_ch);
    dma_channel_cleanup(esp_dma_ch);
    dma_channel_unclaim(esp_dma_ch);
}

// Send len bytes to the ESP over PIO1 TX. Returns 1 on timeout. If cont is nonzero the receive
// side is left armed for cont bytes, otherwise the state machines are reset.
int __not_in_flash_func(qspi_send)(const void *src, uint32_t len, uint32_t cont)
{
    pio_sm_set_enabled(pio1, qspi_in_sm, false);
    pio_sm_set_consecutive_pindirs(pio1, qspi_out_sm, 12, 4, true);
    pio_sm_put_blocking(pio1, qspi_out_status_sm, len * 2 - 1);
    pio_sm_exec_wait_blocking(pio1, qspi_out_status_sm, 0x80a0);
    pio_sm_exec_wait_blocking(pio1, qspi_out_status_sm, 0xa027);
    pio_set_sm_mask_enabled(pio1, 1u << qspi_out_sm, true);
    pio_set_sm_mask_enabled(pio1, 1u << qspi_out_status_sm, true);
    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio1, qspi_out_sm, true));
    channel_config_set_bswap(&c, true);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(ch, &c, &pio1->txf[qspi_out_sm], src, len >> 2, true);
    gpio_put(20, 0);
    while (gpio_get(17))
        ;
    gpio_put(20, 1);
    absolute_time_t t0 = get_absolute_time();
    while (pio_sm_is_rx_fifo_empty(pio1, qspi_out_status_sm)) {
        if (absolute_time_diff_us(t0, get_absolute_time()) > 0x3d0ce7)
            goto timeout;
    }
    (void)pio_sm_get_blocking(pio1, qspi_out_status_sm);
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
    esp_remaining = cont;
    if (cont != 0) {
        pio_sm_set_enabled(pio1, qspi_out_sm, false);
        pio_sm_set_enabled(pio1, qspi_out_status_sm, false);
        pio_sm_set_consecutive_pindirs(pio1, qspi_in_sm, 12, 4, false);
        pio_sm_set_enabled(pio1, qspi_in_sm, true);
    } else {
        qspi_reset_sms();
    }
    return 0;
timeout:
    puts("QSPI timeout!");
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
    qspi_reset_sms();
    return 1;
}
