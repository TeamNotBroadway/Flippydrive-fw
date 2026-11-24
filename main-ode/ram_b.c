// RAM-resident functions of main-ode, second range: audio buffer state, ESP DMA transfers. Names are ours.
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "main_ode.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

extern volatile uint8_t aud_buf_state[9];
extern critical_section_t aud_cs;

void __not_in_flash_func(aud_set_buf_state)(uint8_t buf, uint8_t state)
{
    critical_section_enter_blocking(&aud_cs);
    aud_buf_state[buf] = state;
    critical_section_exit(&aud_cs);
}

void __not_in_flash_func(aud_reset_bufs)(void)
{
    for (int i = 0; i < 9; i++)
        aud_set_buf_state(i, 0);
}

extern mutex_t link_mutex, flash_mutex;
struct esp_rx_buf esp_rx;
__attribute__((section(".mutex_array"))) mutex_t link_mutex;
extern uint qspi_out_sm, qspi_out_status_sm, qspi_in_sm;
extern uint qspi_out_offset, qspi_out_status_offset, qspi_in_offset;
extern uint32_t esp_dma_ch;
extern volatile uint32_t esp_remaining;

void __not_in_flash_func(esp_dma_wait)(void)
{
    dma_channel_wait_for_finish_blocking(esp_dma_ch);
    dma_channel_cleanup(esp_dma_ch);
    dma_channel_unclaim(esp_dma_ch);
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
    if (!async)
        esp_dma_wait();
}

#include "log.h"

// Discard whatever the ESP still has queued for the current transfer.
void __not_in_flash_func(esp_drain)(void)
{
    if (esp_remaining == 0)
        return;
    int ch = dma_claim_unused_channel(true);
    esp_dma_ch = ch;
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio1, qspi_in_sm, false));
    channel_config_set_bswap(&c, true);
    channel_config_set_high_priority(&c, true);
    uint32_t sink;
    LOG("QSPI", "Aborting %d\n", esp_remaining);
    while (esp_remaining != 0) {
        uint32_t n = esp_remaining < 0x4000 ? esp_remaining : 0x4000;
        LOG("QSPI", "Abort read %x %x\n", esp_remaining, n);
        dma_channel_configure(esp_dma_ch, &c, &sink, &pio1->rxf[qspi_in_sm], n >> 2, true);
        gpio_put(20, 0);
        while (gpio_get(17))
            ;
        gpio_put(20, 1);
        esp_remaining -= n;
        dma_channel_wait_for_finish_blocking(esp_dma_ch);
    }
    dma_channel_cleanup(esp_dma_ch);
    dma_channel_unclaim(esp_dma_ch);
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

#include "hardware/uart.h"
#include "hardware/timer.h"
extern bool esp_booted;

// Send len bytes to the ESP over PIO1 TX. Returns 1 on timeout. If cont is nonzero the receive
// side is left armed for cont bytes, otherwise the state machines are reset.
int __not_in_flash_func(qspi_send)(const void *src, uint32_t len, uint32_t cont)
{
    uint32_t words = len + 3;
    pio_sm_set_enabled(pio1, qspi_in_sm, false);
    pio_sm_set_consecutive_pindirs(pio1, qspi_out_sm, 12, 4, true);
    while (pio_sm_is_tx_fifo_full(pio1, qspi_out_status_sm))
        ;
    pio_sm_put(pio1, qspi_out_status_sm, (words & ~3u) * 2 - 1);
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
    dma_channel_configure(ch, &c, &pio1->txf[qspi_out_sm], src, words >> 2, true);
    if (!esp_booted) {
        uart_init(uart0, 115200);
        gpio_set_function(13, GPIO_FUNC_UART);
        hw_set_bits(&uart0_hw->cr, uart0_hw->cr & 0xc000);
        uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
        uart_set_fifo_enabled(uart0, false);
        puts("Waiting for ESP to boot");
        do {
            while (!uart_is_readable(uart0))
                ;
            while (!uart_is_readable(uart0))
                ;
        } while ((uart0_hw->dr & 0xff) != 'e');
        puts("ESP is ready");
        gpio_set_function(13, GPIO_FUNC_PIO1);
        esp_booted = true;
    }
    gpio_put(20, 0);
    while (gpio_get(17))
        ;
    gpio_put(20, 1);
    uint64_t t0 = time_us_64();
    while (pio_sm_is_rx_fifo_empty(pio1, qspi_out_status_sm)) {
        if (likely(time_us_64() - t0 > 0x3d0ce7)) {
            puts("QSPI timeout!");
            dma_channel_cleanup(ch);
            dma_channel_unclaim(ch);
            qspi_reset_sms();
            return 1;
        }
    }
    (void)pio_sm_get(pio1, qspi_out_status_sm);
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
    esp_remaining = cont;
    if ((cont == 0)) {
        qspi_reset_sms();
    } else {
        pio_sm_set_enabled(pio1, qspi_out_status_sm, false);
        pio_sm_set_enabled(pio1, qspi_out_sm, false);
        pio_sm_set_consecutive_pindirs(pio1, qspi_in_sm, 12, 4, false);
        pio_sm_set_enabled(pio1, qspi_in_sm, true);
    }
    return 0;
}
