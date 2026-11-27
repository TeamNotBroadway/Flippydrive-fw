// RAM-resident DI worker (core 1), DI command handlers and flash write, first part. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/sem.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/watchdog.h"
#include "ff.h"
#include "diskio.h"
#include "log.h"
#include "ioexp.h"
#include "flippy.h"

extern uint di_sm;
extern uint di_offset;
extern uint32_t di_error_status;
extern semaphore_t di_sem;
extern mutex_t flash_mutex;
extern const struct pio_program ai_program;
extern uint ai_sm;
extern uint ai_offset;
extern DIR root_dir;
extern FIL di_file;
extern int di_dma_chan;
uint8_t di_state;
uint8_t root_dir_res;
uint8_t root_dir_open;
extern int mount_h2;
extern int mount_h1;
extern bool disc_valid(int v);
extern int di_dispatch(uint32_t a, uint32_t b, uint32_t c);
extern void di_setup(void);
void di_dma_irq(void);
void di_gpio_irq(uint gpio, uint32_t events);

// DMA the next len bytes of di_file to the DI state machine, 512 at a time; zeros past EOF.
void __not_in_flash_func(di_stream_file)(uint32_t len)
{
    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_dreq(&c, pio_get_dreq(pio0, di_sm, true));
    dma_channel_configure(ch, &c, &pio0->txf[di_sm], NULL, 0, false);
    if (len != 0) {
        uint8_t buf[512];
        do {
            uint32_t n = len > 512 ? 512 : len;
            UINT got;
            if (f_read(&di_file, buf, n, &got) != FR_OK) {
                LOG("DI", "FLASH READ ERROR\n");
                return;
            }
            if (got == 0) {
                got = n;
                memset(buf, 0, (int)n);
            }
            uint32_t words = (got + 3) >> 2;
            dma_channel_set_read_addr(ch, buf, false);
            len -= got;
            dma_channel_set_trans_count(ch, words, true);
            dma_channel_wait_for_finish_blocking(ch);
        } while (len != 0);
    }
    dma_channel_cleanup(ch);
    dma_channel_unclaim(ch);
}

void __not_in_flash_func(di_dma_irq)(void)
{
    dma_channel_cleanup(di_dma_chan);
    dma_channel_unclaim(di_dma_chan);
    di_dma_chan = -1;
    sem_release(&di_sem);
}

void __not_in_flash_func(di_ack_error)(void)
{
    if (di_error_status != 0) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
}

DRESULT __not_in_flash_func(disk_write)(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    DRESULT res = RES_PARERR;
    if (pdrv == 0) {
    mutex_enter_blocking(&flash_mutex);
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    uint32_t off = (sector + 0x300) << 12;
    flash_range_erase(off, count << 12);
    flash_range_program(off, buf, count << 12);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    mutex_exit(&flash_mutex);
    res = RES_OK;
    }
    return res;
}

int __not_in_flash_func(cmd_request_error)(void)
{
    LOG("DI", "ERROR_STATUS\n");
    uint32_t err = di_error_status;
    pio_sm_put_blocking(pio0, di_sm,
                        di_state | (err << 24) | (err >> 24) | ((err << 8) & 0xff0000) | ((err >> 8) & 0xff00));
    if (err != 0) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
    return 0;
}

int __not_in_flash_func(cmd_motor_stop)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "MOTOR_STOP\n");
    di_state = 4;
    if (di_error_status != 0) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
    if (disc_valid(mount_h2)) {
        int cur = mount_h1;
        sleep_ms(5);
        ioexp_set_pin(6, true);
        mount_h1 = mount_h2;
        mount_h2 = cur;
        sleep_ms(1000);
        ioexp_set_pin(6, false);
        di_state = 2;
    }
    return 0;
}

int __not_in_flash_func(cmd_inquire)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 1);
    pio_sm_put_blocking(pio0, di_sm, 0x26042220);
    pio_sm_put_blocking(pio0, di_sm, 6);
    pio_sm_put_blocking(pio0, di_sm, 0x0c000401);
    for (int i = 0; i < 4; i++)
        pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "INQUIRE\n");
    if (di_error_status != 0) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
    return 0;
}

void __not_in_flash_func(di_worker)(void)
{
    flash_safe_execute_core_init();
    LOG("DI", "Starting up PIO DI\n");
    sem_init(&di_sem, 0, 1);
    irq_set_exclusive_handler(DMA_IRQ_0, di_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    gpio_set_irq_callback(di_gpio_irq);
    irq_set_enabled(IO_IRQ_BANK0, true);
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    ai_offset = pio_add_program(pio1, &ai_program);
    ai_sm = pio_claim_unused_sm(pio1, true);
    LOG("DI", "ai_program offset %d\n", ai_offset);
    di_setup();
    watchdog_hw->ctrl &= ~WATCHDOG_CTRL_ENABLE_BITS;
    ioexp_set_pin(6, false);
    LOG("DI", "Ready\n");
    root_dir_res = f_opendir(&root_dir, "/");
    if (root_dir_res != 0)
        LOG("DI", "Unable to open root directory\n");
    else
        root_dir_open = 1;
    for (;;) {
        uint32_t a = pio_sm_get_blocking(pio0, di_sm);
        uint32_t b, c;
        while (pio_sm_is_rx_fifo_empty(pio0, di_sm))
            tight_loop_contents();
        b = pio0->rxf[di_sm];
        while (pio_sm_is_rx_fifo_empty(pio0, di_sm))
            tight_loop_contents();
        c = pio0->rxf[di_sm];
        gpio_set_irq_enabled(27, GPIO_IRQ_EDGE_FALL, true);
        if (di_dispatch(a, b, c)) {
            pio_sm_put_blocking(pio0, di_sm, 0);
            LOG("DI", "!!!UNHANDLED CMD %02X!!!\n", a >> 24);
        }
    }
}

// Bypass mode: wait for the exit command (0xDC, magic words), restarting the DI state
// machine whenever GPIO 26 is low.
void __not_in_flash_func(bypass_wait)(void)
{
    for (;;) {
        uint sm = di_sm;
        if (pio_sm_get_rx_fifo_level(pio0, sm) > 2) {
            uint32_t a = pio0->rxf[sm];
            uint32_t b = pio0->rxf[sm];
            uint32_t c = pio0->rxf[sm];
            if ((a >> 24) == DI_CMD_BYPASS && b == BYPASS_EXIT_B && c == BYPASS_EXIT_C) {
                LOG("DI", "Exiting Bypass\n");
                return;
            }
            LOG("DI", "CMD,%08X,%08X,%08X\n", a, b, c);
            sm = di_sm;
        }
        if (!gpio_get(26)) {
            pio_sm_set_enabled(pio0, sm, false);
            pio_sm_clear_fifos(pio0, sm);
            pio_sm_restart(pio0, sm);
            pio_sm_exec_wait_blocking(pio0, sm, pio_encode_jmp(di_offset));
            pio_sm_set_enabled(pio0, sm, true);
        }
    }
}
