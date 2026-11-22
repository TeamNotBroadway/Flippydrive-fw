// Bootloader app code: USB-CDC to UART bridge, FatFs flash diskio, I/O expander. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "hardware/i2c.h"
#include "tusb.h"
#include "ff.h"
#include "diskio.h"

__attribute__((section(".mutex_array"))) mutex_t expander_mutex;
uint8_t expander_state;
int16_t bridge_escape;

void expander_set(uint pin, bool on)
{
    mutex_enter_blocking(&expander_mutex);
    uint8_t mask = 1u << pin;
    if (on)
        expander_state = expander_state | mask;
    else
        expander_state = expander_state & (uint8_t)~mask;
    uint8_t buf[2] = {1, expander_state};
    i2c_write_blocking(i2c1, 0x20, buf, 2, false);
    mutex_exit(&expander_mutex);
}

void usb_uart_bridge(void)
{
    uint8_t c;
    do {
        c = 6;
    } while (tud_cdc_n_write(0, &c, 1) == 0);
    expander_set(7, 0);
    expander_set(5, 0);
    expander_set(0, 0);
    gpio_set_function(16, GPIO_FUNC_UART);
    gpio_set_function(17, GPIO_FUNC_UART);
    uart_init(uart0, 115200);
    *(volatile uint32_t *)0x40035030 = uart0_hw->cr & 0xc000;
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, false);
    busy_wait_ms(10);
    expander_set(5, 1);
    for (;;) {
        if (!tud_cdc_n_available(0)) {
            if (uart0_hw->fr & 0x10)
                continue;
            goto drain;
        }
        uint32_t b;
        if (tud_cdc_n_read(0, &c, 1) == 0)
            b = 0xff;
        else
            b = c;
        if (bridge_escape == (int)b) {
            while (uart0_hw->fr & 0x20) {}
            uart0_hw->dr = 0xd;
        }
        while (uart0_hw->fr & 0x20) {}
        uart0_hw->dr = b;
        if (!(uart0_hw->fr & 0x10)) {
        drain:
            while (uart0_hw->fr & 0x10) {}
            c = (uint8_t)uart0_hw->dr;
            tud_cdc_n_write(0, &c, 1);
            tud_cdc_n_write_flush(0);
        }
    }
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT r = 4;
    if (pdrv == 0) {
        memcpy(buff, (const void *)((sector + 0x13300) * 0x1000), count << 12);
        r = 0;
    }
    return r;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT r = 4;
    if (pdrv == 0) {
        multicore_lockout_start_blocking();
        uint32_t irq = save_and_disable_interrupts();
        uint32_t off = (sector + 0x300) * 0x1000;
        flash_range_erase(off, count << 12);
        flash_range_program(off, buff, count << 12);
        restore_interrupts(irq);
        multicore_lockout_end_blocking();
        r = 0;
    }
    return r;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    DRESULT res = RES_PARERR;
    if (pdrv == 0) {
        switch (cmd) {
        case CTRL_SYNC: res = RES_OK; break;
        case GET_SECTOR_COUNT: if (buff) { *(LBA_t *)buff = 0xd00; res = RES_OK; } break;
        case GET_SECTOR_SIZE: if (buff) { *(WORD *)buff = 0x1000; res = RES_OK; } break;
        case GET_BLOCK_SIZE: if (buff) { *(DWORD *)buff = 1; res = RES_OK; } break;
        }
    }
    return res;
}
