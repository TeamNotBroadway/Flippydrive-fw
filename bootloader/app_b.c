// Bootloader app code: ESP32 reset/boot wait, SD update copy, update sequencing, main loop. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/resets.h"
#include "tusb.h"
#include "ff.h"
#include "diskio.h"

#include "../main-ode/flippy.h"
#define REQ_MAGIC ESP_REQ_MAGIC

void expander_set(uint pin, bool on);
void expander_init(void);
void expander_set_config(uint8_t config);
__attribute__((noreturn)) void usb_uart_bridge(void);
#ifdef FD_ESP_EXTRACT
__attribute__((noreturn)) void clean_bridge(void);  /* diagnostic build only, docs/ESP32.md */
#endif
void qspi_init(void);
int install_update(const char *path);
void bootloader_fs_init(void);
void core1_entry(void);
int qspi_send(const void *src, uint32_t len, uint32_t cont);
void esp_dma_read(void *dst, uint32_t len, uint32_t *done, int async);

#include "bootloader.h"

struct fd_boot_status fd_status = {"FDST"};
struct dol_list_t dol_list = {"FDST"};

extern volatile uint8_t esp_ready;
extern volatile uint8_t boot_request;
extern volatile uint8_t update_request;
extern volatile uint8_t force_update;
extern volatile uint8_t factory_flag;

void esp_reset(void)
{
    expander_set(7, 0);
    expander_set(5, 0);
    expander_set(0, 1);
    busy_wait_ms(10);
    expander_set(5, 1);
    busy_wait_ms(10);
    expander_set(7, 1);
}

// Returns true when the ESP32 did not announce itself within 4 s.
bool esp_boot_wait(void)
{
    gpio_set_function(13, GPIO_FUNC_UART);
    uart_init(uart0, 115200);
    *(volatile uint32_t *)0x40035030 = uart0_hw->cr & 0xc000;
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, false);
    absolute_time_t t = make_timeout_time_ms(4000);
    while (to_us_since_boot(get_absolute_time()) < to_us_since_boot(t)) {
        if (uart_is_readable(uart0) && uart_getc(uart0) == 'e') {
            puts("esp booted!");
            return false;
        }
    }
    puts("esp boot timeout");
    return true;
}

__attribute__((noinline, noreturn)) void launch_app(uint32_t offset)
{
    uint32_t base = XIP_BASE + offset;
    systick_hw->csr &= ~1u;
    nvic_hw->icer = 0xffffffff;
    nvic_hw->icpr = 0xffffffff;
    hw_set_bits(&resets_hw->reset, 0xfffbedbf);
    register uint32_t sp asm("r1") = *(volatile uint32_t *)base;
    register uint32_t pc asm("r3") = *(volatile uint32_t *)(XIP_BASE + 4 + offset);
    scb_hw->vtor = base;
    __asm volatile("msr msp, %0 \n bx %1" : : "r"(sp), "r"(pc));
    for (;;)
        __wfi();
}

void cdc_printf(const char *fmt, ...)
{
    static char buf[0x80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    tud_cdc_n_write(0, buf, n);
    tud_cdc_n_write_flush(0);
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv != 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return pdrv != 0;
}

void boot_direct(void)
{
    busy_wait_ms(100);
    puts("Directly booting firmware...");
    multicore_reset_core1();
    watchdog_enable(0x7fffff, 0);
    launch_app(0x80000);
}

void scan_dols(void)
{
    FILINFO fno;
    DIR dir;
    if (f_opendir(&dir, "/") != FR_OK)
        return;
    int n = 0;
    for (int tries = 100; tries != 0; tries--) {
        if (f_readdir(&dir, &fno) != FR_OK)
            break;
        uint8_t attr = fno.fattrib & (AM_DIR | AM_HID);
        if (unlikely(0 != attr))
            continue;
        size_t len = strlen(fno.fname);
        if (len > 4 && fno.fname[len - 4] == '.' && (fno.fname[len - 3] & ~0x20) == 'D' &&
            (fno.fname[len - 2] & ~0x20) == 'O' && (fno.fname[len - 1] & ~0x20) == 'L') {
            strncpy(dol_list.name[n], fno.fname, 0x28);
            dol_list.name[n][0x27] = attr;
            if (unlikely(++n > 7))
                break;
        }
    }
    f_closedir(&dir);
}

struct net_req {
    uint32_t magic;
    uint8_t cmd, arg;
    uint8_t pad[2];
    uint32_t off, len, rest[5];
};

struct open_req {
    uint32_t magic;
    uint8_t cmd;
    uint8_t pad0[0x1b];
    char path[0x100];
    uint8_t pad1;
    uint8_t mode;
    uint8_t pad2[0x1e];
};

struct open_rep {
    uint32_t status;
    uint32_t size_lo, size_hi;
    uint8_t fd;
    uint8_t pad[0x13];
};

#define UPDATE_PATH "/bootloader/update.fpkg"

// 0: new update.fpkg stored; 1: not on the SD card; 2: empty; negative: link or write error.
int fetch_update(void)
{
    uint ret;

    puts("Opening SD card /update.fpkg");
    uint8_t buf[0x1000];
    struct open_rep rep;
    {
        struct open_req q;
        q.magic = REQ_MAGIC;
        q.cmd = IPC_OPEN;
        strncpy(q.path, "/update.fpkg", 0xff);
        q.path[0xff] = 0;
        q.mode = 4;
        ret = qspi_send(&q, 0x140, 0);
        if (0 != ret) {
            ret = 1;
            puts("Unable to send open command");
            return -ret;
        }
        q.magic = REQ_MAGIC;
        q.cmd = IPC_STATUS;
        if (likely(qspi_send(&q, 0x20, 0x20) == 0)) {
            uint got = 0;
            do {
                uint32_t n = 0;
                esp_dma_read((uint8_t *)&rep + got, 0x20 - got, &n, 0);
                got += n;
            } while (got < 0x20);
        }
        if (0 != rep.status) {
            puts("Could not open SD card /update.fpkg, skipping");
            return 1;
        }
    }

    uint32_t remaining = rep.size_lo;
    uint8_t fd = rep.fd;
    printf("Update size: %d\n", (unsigned)remaining);
    fd_status.show_video = 1;
    fd_status.show_progress_bar = 1;
    if (!remaining)
        return 2;

    bool opened = false;
    FIL fil;
    do {
        struct net_req r;
        UINT bw;
        r.magic = REQ_MAGIC;
        r.cmd = IPC_READ;
        r.arg = fd;
        r.off = rep.size_lo - remaining;
        unsigned chunk = remaining > 0x1000 ? 0x1000 : remaining;
        r.len = chunk;
        if (likely(likely(qspi_send(&r, 0x20, chunk) == 0))) {
            int32_t got = 0;
            do {
                int32_t n = 0;
                esp_dma_read(got + buf, chunk - got, &n, 0);
                got += n;
            } while (got < chunk);
        }
        uint64_t size = ((uint64_t)rep.size_hi << 32) | rep.size_lo;
        fd_status.progress = __builtin_bswap16((uint16_t)((uint64_t)(1000 * r.off) / size));
        if (likely(0 == opened)) {
            puts("Opening local FAT " UPDATE_PATH);
            if (f_open(&fil, UPDATE_PATH, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
                ret = 2;
                puts("Could not open local FAT " UPDATE_PATH ", skipping");
                return -ret;
            }
        }
        if (unlikely(f_write(&fil, buf, r.len, &bw) != FR_OK || r.len > bw)) {
            puts("Unable to write " UPDATE_PATH);
            ret = 3;
            ret = -ret;
            f_close(&fil);
            return ret;
        }
        remaining -= chunk;
        opened = true;
    } while (remaining);
    f_close(&fil);
    {
        struct net_req r;
        r.magic = REQ_MAGIC;
        r.cmd = IPC_CLOSE;
        r.arg = fd;
        qspi_send(&r, 0x20, 0);
    }
    return 0;
}

#define ESP_DOWNLOAD_MODE()   \
    do {                      \
        expander_set(7, 0);   \
        expander_set(5, 0);   \
        expander_set(0, 0);   \
        busy_wait_ms(10);     \
        expander_set(5, 1);   \
    } while (0)

#define SET_LINE1(s) strcpy(fd_status.text, s)
#define SET_LINE2(s) strcpy(fd_status.subtext, s)

// Update the ESP32 from the SD card or the local FAT, then hand off to the main firmware.
void update_and_boot(void)
{
    bool unchanged;
    int rc;

    qspi_init();
    uint8_t c;
    SET_LINE1("Waiting for ESP Boot...");
    if (!likely(esp_boot_wait())) {
        gpio_set_function(13, GPIO_FUNC_PIO1);
        busy_wait_ms(100);
        SET_LINE1("Preparing SD card updates");
        rc = fetch_update();
        if (unlikely(rc != 0)) {
            unchanged = rc == 2;
            SET_LINE1("Update complete, press START");
            f_unlink("/forceupdate");
            f_unlink("/forceupdate.txt");
            goto report;
        }
        SET_LINE1("Resetting ESP32");
        ESP_DOWNLOAD_MODE();
        SET_LINE1("Performing update");
        if (!likely(install_update(UPDATE_PATH) == 0)) {
            memcpy(fd_status.text, "Update FAILED, replace update.fpkg or do USB recovery", 0x36);
        } else {
            SET_LINE1("Update complete, press START");
            f_unlink("/forceupdate");
            f_unlink("/forceupdate.txt");
        }
    } else {
        SET_LINE2("Attempting ESP reflash");
        SET_LINE1("Preparing SD card updates");
        SET_LINE1("Resetting ESP32");
        ESP_DOWNLOAD_MODE();
        unchanged = false;
        SET_LINE1("Performing update");
        if (unlikely(install_update(UPDATE_PATH) != 0)) {
            memcpy(fd_status.text, "Update FAILED, replace update.fpkg or do USB recovery", 0x36);
            goto report;
        }
        SET_LINE1("ESP programmed, rebooting");
        busy_wait_ms(100);
        gpio_set_function(13, GPIO_FUNC_PIO1);
        expander_set(5, 0);
        busy_wait_ms(100);
        while (uart_is_readable(uart0))
            (void)uart_getc(uart0);
        busy_wait_ms(100);
        esp_reset();
        busy_wait_ms(4000);
        SET_LINE1("Preparing SD card updates");
        if (unlikely(esp_boot_wait())) {
            SET_LINE1("ESP boot FAILURE, do USB recovery");
            goto report;
        }
        gpio_set_function(13, GPIO_FUNC_PIO1);
        rc = fetch_update();
        if (rc != 0) {
            SET_LINE1("Update complete, press START");
            unchanged = rc == 2;
            f_unlink("/forceupdate");
            f_unlink("/forceupdate.txt");
            goto report;
        }
        SET_LINE1("Resetting ESP32");
        ESP_DOWNLOAD_MODE();
        SET_LINE1("Performing update");
        if (unlikely(install_update(UPDATE_PATH) == 0)) {
            SET_LINE1("Update complete, press START");
            f_unlink("/forceupdate");
            f_unlink("/forceupdate.txt");
        } else {
            memcpy(fd_status.text, "Update FAILED, replace update.fpkg or do USB recovery", 0x36);
        }
    }

report:
    fd_status.progress = 0xffff;
    c = unchanged ? 0x14 : 0x15;
    tud_cdc_n_write(0, &c, 1);
    cdc_printf("Booting main firmware...\n");
    busy_wait_ms(10);
    esp_reset();
    while (!esp_ready)
        ;
    SET_LINE2("Main application handoff triggered");
    multicore_reset_core1();
    watchdog_enable(0x7fffff, 0);
    launch_app(0x80000);
}

// All pins to inputs with the activity pin driving expander pin 4; nothing in the image calls it.
void bypass_pins_init(void)
{
    gpio_set_dir(0, GPIO_IN);
    gpio_set_dir(1, GPIO_IN);
    gpio_set_dir(2, GPIO_IN);
    gpio_set_dir(3, GPIO_IN);
    gpio_set_dir(4, GPIO_IN);
    gpio_set_dir(5, GPIO_IN);
    gpio_set_dir(6, GPIO_IN);
    gpio_set_dir(7, GPIO_IN);
    gpio_set_dir(27, GPIO_IN);
    gpio_set_dir(28, GPIO_IN);
    gpio_set_dir(20, GPIO_IN);
    gpio_set_dir(25, GPIO_IN);
    gpio_set_dir(17, GPIO_IN);
    gpio_set_dir(21, GPIO_IN);
    gpio_set_dir(23, GPIO_IN);
    gpio_set_dir(22, GPIO_IN);
    gpio_set_dir(24, GPIO_IN);
    gpio_set_dir(29, GPIO_IN);
    gpio_set_dir(25, GPIO_IN);
    gpio_set_dir(17, GPIO_IN);
    gpio_set_dir(21, GPIO_IN);
    gpio_set_dir(23, GPIO_IN);
    gpio_set_function(0, GPIO_FUNC_SIO);
    gpio_set_function(1, GPIO_FUNC_SIO);
    gpio_set_function(2, GPIO_FUNC_SIO);
    gpio_set_function(3, GPIO_FUNC_SIO);
    gpio_set_function(4, GPIO_FUNC_SIO);
    gpio_set_function(5, GPIO_FUNC_SIO);
    gpio_set_function(6, GPIO_FUNC_SIO);
    gpio_set_function(7, GPIO_FUNC_SIO);
    gpio_set_function(27, GPIO_FUNC_SIO);
    gpio_set_function(28, GPIO_FUNC_SIO);
    gpio_set_function(21, GPIO_FUNC_SIO);
    gpio_set_function(25, GPIO_FUNC_SIO);
    gpio_set_function(22, GPIO_FUNC_SIO);
    gpio_set_function(24, GPIO_FUNC_SIO);
    gpio_set_function(29, GPIO_FUNC_SIO);
    i2c_init(i2c1, 400000);
    gpio_set_function(18, GPIO_FUNC_I2C);
    gpio_set_function(19, GPIO_FUNC_I2C);
    gpio_pull_up(18);
    gpio_pull_up(19);
    gpio_init(26);
    gpio_set_dir(26, GPIO_IN);
    gpio_pull_up(26);
    expander_init();
    expander_set(7, 0);
    expander_set(5, 0);
    expander_set(0, 1);
    expander_set(1, 0);
    expander_set_config(0x5c);
    for (;;)
        expander_set(4, gpio_get(26));
}

int main(void)
{
    *(volatile uint32_t *)(PADS_BANK0_BASE + 0x7c) = 0;
    *(volatile uint32_t *)(PADS_BANK0_BASE + 0x80) = 0;
    gpio_set_function(0, GPIO_FUNC_PIO0);
    gpio_set_function(1, GPIO_FUNC_PIO0);
    gpio_set_function(2, GPIO_FUNC_PIO0);
    gpio_set_function(3, GPIO_FUNC_PIO0);
    gpio_set_function(4, GPIO_FUNC_PIO0);
    gpio_set_function(5, GPIO_FUNC_PIO0);
    gpio_set_function(6, GPIO_FUNC_PIO0);
    gpio_set_function(7, GPIO_FUNC_PIO0);
    for (int i = 0; i < 8; i++)
        pads_bank0_hw->io[i] |= 0xf;

    gpio_init(27);
    gpio_set_dir(27, GPIO_IN);
    gpio_init(28);
    gpio_put(28, 1);
    gpio_set_dir(28, GPIO_OUT);
    gpio_init(12);
    gpio_init(13);
    gpio_init(14);
    gpio_init(15);
    gpio_init(20);
    gpio_put(20, 1);
    gpio_set_dir(20, GPIO_OUT);
    gpio_init(25);
    gpio_set_dir(25, GPIO_IN);
    gpio_init(17);
    gpio_set_dir(17, GPIO_IN);
    gpio_pull_up(17);
    gpio_init(21);
    gpio_set_dir(21, GPIO_IN);
    gpio_init(23);
    gpio_put(23, 0);
    gpio_set_dir(23, GPIO_OUT);

    i2c_init(i2c1, 400000);
    gpio_set_function(18, GPIO_FUNC_I2C);
    gpio_set_function(19, GPIO_FUNC_I2C);
    gpio_pull_up(18);
    gpio_pull_up(19);

    gpio_init(29);
    gpio_put(29, 0);
    gpio_set_dir(29, GPIO_OUT);
    gpio_init(26);
    gpio_set_dir(26, GPIO_IN);
    gpio_pull_up(26);

    expander_init();
    expander_set(7, 0);
    expander_set(6, 0);
    expander_set(2, 1);
    expander_set(4, 0);
    expander_set(1, 1);
    expander_set(5, 0);
    expander_set(0, 1);
    expander_set_config(8);

    multicore_launch_core1_with_stack(core1_entry, malloc(0x2800), 0x2800);
    stdio_usb_init();
    puts("BOOTED");
    fd_status.show_progress_bar = 0;
    SET_LINE1("ESP Startup...");
    esp_reset();
    puts("Mounting local filesystem");
    bootloader_fs_init();
    if (f_stat("/forceupdate", NULL) == FR_OK)
        force_update = 1;
    if (f_stat("/forceupdate.txt", NULL) == FR_OK)
        force_update = 1;
    if (f_stat("/factory", NULL) == FR_OK)
        factory_flag = 1;
    if (!force_update)
        scan_dols();
    for (;;) {
        if (stdio_usb_connected()) {
            uint8_t c = 0x13;
            tud_cdc_n_write(0, &c, 1);
            if (tud_cdc_n_read(0, &c, 1)) {
                if (c == 0x12)
                    usb_uart_bridge();
#ifdef FD_ESP_EXTRACT
                if (c == 0x11)
                    clean_bridge();
#endif
                if (c == 0x13)
                    update_and_boot();
            }
        }
        if (update_request)
            update_and_boot();
        if (boot_request)
            boot_direct();
    }
}
