// Bootloader app code: FAT mount, update.fpkg installer, I/O expander init, DI command table, QSPI PIO setup. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/busctrl.h"
#include "hardware/flash.h"
#include "ff.h"

int connect_to_target(uint32_t higher_baudrate);
int esp_flash_start(uint32_t addr, uint32_t len, uint32_t block);
int esp_flash_write(uint32_t addr, void *data, uint32_t len);
void esp_flash_finish(void);
void loader_port_reset_target(void);

extern mutex_t expander_mutex;
extern uint8_t expander_state;

#include "bootloader.h"

#define FPKG_MAX 16
struct fpkg_header {
    char magic[4];
    uint32_t unknown;
    uint32_t count;
    uint32_t offset[FPKG_MAX];
    uint32_t size[FPKG_MAX];
    uint8_t pad[512 - 12 - 8 * FPKG_MAX];
};

struct fim_header {
    char magic[4];
    uint32_t version;
    uint32_t chip;
    uint32_t address;
    uint32_t length;
    uint8_t slot;
    uint8_t pad[3];
    char name[64];
};

#define CHIP_RP2040 0xe48bff56
#define CHIP_ESP32 0x1c5f21b0
#define CHIP_FILE 0xa8a66268

static FATFS fs;
static DIR bootloader_dir;
static uint8_t xfer_buf[512];

__attribute__((noinline)) FRESULT __not_in_flash_func(make_dirs)(const char *path);
__attribute__((noinline)) void __not_in_flash_func(hexdump)(const uint8_t *buf, uint32_t len);

void __not_in_flash_func(bootloader_fs_init)(void)
{
    puts("Mounting internal FAT");
    FRESULT res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        printf("FAT filesystem failed to initialize, formatting (%d)\n", res);
        res = f_mkfs("", 0, 0, 0x4000);
        if (res != FR_OK) {
            printf("Unable to create FAT filesystem, aborting (%d)\n", res);
            for (;;)
                ;
        }
        res = f_mount(&fs, "", 1);
        if (res != FR_OK) {
            printf("FAT filesystem failed to initialize, aborting (%d)\n", res);
            for (;;)
                ;
        }
    }
    puts("Internal FAT Mounted");
    puts("Creating bootloader paths");
    res = make_dirs("/bootloader");
    if (res != FR_OK) {
        printf("Could not create bootloader install dirs %d\n", res);
        for (;;)
            sleep_ms(10);
    }
    res = f_opendir(&bootloader_dir, "/bootloader");
    if (res != FR_OK) {
        puts("Could not open bootloader dir");
        for (;;)
            sleep_ms(10);
    }
    puts("Created bootloader paths");
}

__attribute__((noinline)) FRESULT __not_in_flash_func(make_dirs)(const char *path)
{
    char acc[256];
    DIR dir;
    char tmp[256];
    acc[0] = 0;
    strncpy(tmp, path, 255);
    tmp[255] = 0;
    char *tok = strtok(tmp, "/");
    if (unlikely(!tok))
        return FR_OK;
    do {
        size_t n = strlen(acc);
        acc[n] = '/';
        strcpy(acc + n + 1, tok);
        FRESULT res = f_opendir(&dir, acc);
        if (unlikely(res == FR_OK)) {
            f_closedir(&dir);
        } else if (unlikely(res == FR_NO_PATH)) {
            res = f_mkdir(acc);
            if (unlikely(res != FR_OK))
                return res;
        } else {
            return res;
        }
        tok = strtok(NULL, "/");
    } while (tok);
    return FR_OK;
}

__attribute__((noinline)) void __not_in_flash_func(esp_fast_start)(void)
{
    puts("Starting ESP FAST");
    connect_to_target(921600);
}

__attribute__((noinline)) void __not_in_flash_func(hexdump)(const uint8_t *buf, uint32_t size)
{
    char ascii[17];
    uint32_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i) {
        cdc_printf("%02X ", buf[i]);
        if (buf[i] >= ' ' && buf[i] <= '~')
            ascii[i % 16] = buf[i];
        else
            ascii[i % 16] = '.';
        if ((i + 1) % 8 == 0 || i + 1 == size) {
            cdc_printf(" ");
            if ((i + 1) % 16 == 0) {
                cdc_printf("|  %s \n", ascii);
            } else if (i + 1 == size) {
                ascii[(i + 1) % 16] = '\0';
                if ((i + 1) % 16 <= 8)
                    cdc_printf(" ");
                for (j = (i + 1) % 16; j < 16; ++j)
                    cdc_printf("   ");
                cdc_printf("|  %s \n", ascii);
            }
        }
    }
}

__attribute__((noinline)) int __not_in_flash_func(flash_esp32)(FIL *f, uint32_t off, uint32_t addr, uint32_t len, bool finish)
{
    if (len == 0)
        return -1;
    if (len & 0x1ff)
        return -2;
    int res = f_lseek(f, off + 0x200);
    if (res)
        return res;
    fd_status.show_progress_bar = 1;
    fd_status.progress = 0;
    strcpy(fd_status.text, "Erasing ESP32");
    res = esp_flash_start(addr, len, 0x200);
    if (res)
        return res;
    strcpy(fd_status.text, "Programming ESP32");
    uint32_t blocks = len >> 9;
    uint32_t end = blocks * 1000;
    uint32_t progress = 0;
    do {
        UINT br;
        res = f_read(f, xfer_buf, 0x200, &br);
        if (unlikely(res)) {
            cdc_printf("Read ESP32 ERROR %d\n", res);
            return res;
        }
        if (br == 0)
            break;
        fd_status.progress = __builtin_bswap16(progress / blocks);
        res = esp_flash_write(addr, xfer_buf, 0x200);
        if (unlikely(res)) {
            cdc_printf("Write ESP32 ERROR\n");
            return res;
        }
        progress += 1000;
        addr += 0x200;
    } while (progress != end);
    strcpy(fd_status.text, "ESP32 Programmed");
    if (finish) {
        esp_flash_finish();
        loader_port_reset_target();
    }
    return 0;
}

__attribute__((noinline)) int __not_in_flash_func(flash_rp2040)(FIL *f, uint32_t off, uint32_t addr, uint32_t len)
{
    uint32_t ret;
    if (unlikely(len == 0)) {
        ret = 1;
    } else if (unlikely(len & 0x1ff)) {
        ret = 2;
    } else {
        ret = f_lseek(f, 0x200 + off);
        if (likely(ret == 0)) {
            unsigned flash_off = addr - XIP_BASE;
            uint32_t blocks = len >> 9;
            fd_status.show_progress_bar = 1;
            fd_status.progress = 0;
            strcpy(fd_status.text, "Erasing RP2040");
            uint progress = 0;
            cdc_printf("Clearing from +%x\n", flash_off);
            flash_range_erase(flash_off, (len + 0xfff) & ~0xfffu);
            strcpy(fd_status.text, "Programming RP2040");
            uint32_t end = 1000 * blocks;
            do {
                UINT br;
                ret = f_read(f, xfer_buf, 0x200, &br);
                if (unlikely(ret || br != 0x200)) {
                    printf("Unable to read update file during 2040 programming %d\n", (int)ret);
                    break;
                }
                fd_status.progress = __builtin_bswap16(progress / blocks);
                flash_range_program(flash_off, xfer_buf, 0x200);
                progress += 1000;
                flash_off += 0x200;
            } while (progress != end);
        }
    }
    return ret;
}

__attribute__((noinline)) int __not_in_flash_func(install_file)(FIL *f, uint32_t off, uint32_t len, const char *name)
{
    FIL out;
    if (len == 0)
        return 1;
    uint32_t res = f_open(&out, name, FA_CREATE_ALWAYS | FA_WRITE);
    if (unlikely(0 != res))
        return res;
    res = f_lseek(f, off + 0x200);
    if (res != 0) {
        f_close(&out);
        return res;
    }
    fd_status.show_progress_bar = 1;
    fd_status.progress = 0;
    strcpy(fd_status.text, "Updating Files...");
    uint32_t pos = 0;
    do {
        UINT n = len - pos;
        UINT br, bw;
        if (unlikely(n > 0x200))
            n = 0x200;
        res = f_read(f, xfer_buf, n, &br);
        if (res) {
            printf("Unable to read update file during FAT programming %d\n", (int32_t)res);
            f_close(&out);
            return res;
        }
        fd_status.progress = __builtin_bswap16(pos * 1000 / len);
        res = f_write(&out, xfer_buf, br, &bw);
        if (unlikely(res || bw != br)) {
            printf("Unable to write Flash file during FAT programming %d\n", res);
            f_close(&out);
            return res;
        }
        pos += bw;
    } while (pos < len);
    f_close(&out);
    return 0;
}

int __not_in_flash_func(install_update)(const char *path)
{
    uint8_t buf[512];
    FIL f;
    cdc_printf("Installing update %s\n", path);
    struct fpkg_header hdr;
    fd_status.show_video = 1;
    fd_status.show_progress_bar = 1;
    fd_status.progress = 0;
    UINT br;
    if (f_open(&f, path, FA_READ))
        return 1;
    if (likely(f_read(&f, buf, 0x200, &br)))
        return 1;
    if (unlikely(br != 0x200))
        return 1;
    memcpy(&hdr, buf, br);
    hexdump((uint8_t *)&hdr, br);
    uint32_t count = hdr.count;
    cdc_printf("Update contains %d parts\n", count);
    esp_fast_start();
    for (uint32_t i = 0; i < count; i++) {
        int32_t len = hdr.size[i];
        uint off = hdr.offset[i];
        struct fim_header *img = (struct fim_header *)buf;
        cdc_printf("DEBUG image_length = 0x%x\n", len);
        if (len == 0)
            continue;
        if (len > 0x1000000) {
            cdc_printf("ERROR image size too large\n");
            return 1;
        }
        cdc_printf("DEBUG image_offset = 0x%x\n", off);
        unsigned res = f_lseek(&f, off);
        if (res) {
            cdc_printf("img bad seek %d\n", res);
            return 1;
        }
        res = f_read(&f, buf, 0x200, NULL);
        if (0 != res) {
            cdc_printf("img bad first read\n");
            return 1;
        }
        cdc_printf("DEBUG IMG: magic = %.4s\n", img->magic);
        cdc_printf("DEBUG IMG: version_num = %08x\n", img->version);
        cdc_printf("DEBUG IMG: slot = %d\n", img->slot);
        cdc_printf("DEBUG IMG: chip = %08x\n", img->chip);
        cdc_printf("DEBUG IMG: address = %08x\n", img->address);
        cdc_printf("DEBUG IMG: length = %08x\n", img->length);
        cdc_printf("DEBUG IMG: name = %s\n", img->name);
        strncpy(fd_status.subtext, img->name, 63);
        fd_status.subtext[63] = 0;
        if (*(uint32_t *)img->magic != 0x314d4946) {
            puts("Invalid flash image segment!");
            continue;
        }
        if (CHIP_RP2040 == img->chip) {
            cdc_printf("Installing RP2040: %s\n", img->name);
            flash_rp2040(&f, off, img->address, img->length);
        } else if (img->chip == CHIP_ESP32) {
            cdc_printf("Installing ESP32: %s\n", img->name);
            res = flash_esp32(&f, off, img->address, img->length, false);
            if (0 != res) {
                cdc_printf("ESP flash failure %d\n", (uint32_t)res);
                return 1;
            }
        } else if (CHIP_FILE == img->chip) {
            cdc_printf("Installing FILE: %s\n", img->name);
            res = install_file(&f, off, img->length, img->name);
            if (res != 0) {
                cdc_printf("File copy failure %d\n", res);
                return 1;
            }
        }
        cdc_printf("Installed %s\n", img->name);
    }
    f_close(&f);
    return 0;
}

void __not_in_flash_func(expander_init)(void)
{
    mutex_enter_blocking(&expander_mutex);
    uint8_t buf[2] = {3, 0xff};
    expander_state = 0;
    i2c_write_blocking(i2c1, 0x20, buf, 2, false);
    buf[0] = 2;
    buf[1] = 0;
    i2c_write_blocking(i2c1, 0x20, buf, 2, false);
    buf[0] = 1;
    buf[1] = expander_state;
    i2c_write_blocking(i2c1, 0x20, buf, 2, false);
    mutex_exit(&expander_mutex);
}

void __not_in_flash_func(expander_set_config)(uint8_t config)
{
    mutex_enter_blocking(&expander_mutex);
    uint8_t buf[2] = {3, config};
    i2c_write_blocking(i2c1, 0x20, buf, 2, false);
    mutex_exit(&expander_mutex);
}

int (*cmd_handlers[256])(void);

void __attribute__((noinline)) __not_in_flash_func(set_cmd_handler)(int cmd, int (*handler)(void))
{
    cmd_handlers[cmd] = handler;
}

int __not_in_flash_func(di_dispatch)(uint32_t a, uint32_t b, uint32_t c)
{
    int (*handler)(void) = cmd_handlers[a >> 24];
    if (handler == NULL)
        return 1;
    ((int (*)(uint32_t, uint32_t, uint32_t))handler)(a, b, c);
    return 0;
}

static const uint16_t di_dir_instructions[] = {0x3e9a, 0x209b, 0x3e1b};
const struct pio_program di_dir_program = {di_dir_instructions, 3, -1};
// DI bus program, raw words from the image (0x20018eb8); no .pio source yet.
static const uint16_t di_instructions[] = {
    0x309a, 0x301b, 0x2019, 0x2099, 0x4008, 0x0042, 0x3019, 0x3099, 0x5008, 0x1086, 0x309b,
    0x3019, 0x6308, 0xf321, 0x6308, 0xb142, 0x10d2, 0x100c, 0xb042, 0x6308, 0x1353, 0x100b,
};
const struct pio_program di_program = {di_instructions, 22, -1};

static inline pio_sm_config di_dir_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 1, offset + 2);
    sm_config_set_sideset(&c, 4, false, true);
    return c;
}

void __not_in_flash_func(di_dir_sm_init)(uint sm, uint offset, uint pin)
{
    pio_sm_config c = di_dir_program_get_default_config(offset);
    pio_sm_set_consecutive_pindirs(pio0, sm, pin, 4, false);
    c.pinctrl = (c.pinctrl & ~0x7c00u) | (pin << 10);
    pio_sm_init(pio0, sm, offset, &c);
    pio_sm_set_enabled(pio0, sm, true);
}

int cmd_request_error(void);
int cmd_motor_stop(void);
int cmd_inquire(void);
int cmd_seek(void);
int cmd_read(void);
int bypass_enter(void);
int cmd_audio_config(void);
int cmd_fd_status(void);
int cmd_dol_list(void);

static void __attribute__((constructor)) register_e0(void) { set_cmd_handler(DI_CMD_REQUEST_ERROR, cmd_request_error); }
static void __attribute__((constructor)) register_e3(void) { set_cmd_handler(DI_CMD_STOP_MOTOR, cmd_motor_stop); }
static void __attribute__((constructor)) register_12(void) { set_cmd_handler(DI_CMD_INQUIRY, cmd_inquire); }
static void __attribute__((constructor)) register_ab(void) { set_cmd_handler(DI_CMD_SEEK, cmd_seek); }
static void __attribute__((constructor)) register_a8(void) { set_cmd_handler(DI_CMD_READ, cmd_read); }
static void __attribute__((constructor)) register_dc(void) { set_cmd_handler(DI_CMD_BYPASS, bypass_enter); }
static void __attribute__((constructor)) register_e4(void) { set_cmd_handler(DI_CMD_AUDIO_CONFIG, cmd_audio_config); }
static void __attribute__((constructor)) register_b4(void) { set_cmd_handler(DI_CMD_FD_STATUS, cmd_fd_status); }
static void __attribute__((constructor)) register_b3(void) { set_cmd_handler(DI_CMD_DOL_LIST, cmd_dol_list); }

#define qspi_out_status_wrap_target 0
#define qspi_out_status_wrap 5
static const uint16_t qspi_out_status_program_instructions[] = {
    0x2011, 0x2010, 0x2090, 0x0141, 0xa0c5, 0x8020,
};
static const struct pio_program qspi_out_status_program = {
    .instructions = qspi_out_status_program_instructions,
    .length = 6,
    .origin = -1,
    .pio_version = 0,
};

static inline pio_sm_config qspi_out_status_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + qspi_out_status_wrap_target, offset + qspi_out_status_wrap);
    return c;
}

#define qspi_out_wrap_target 1
#define qspi_out_wrap 3
static const uint16_t qspi_out_program_instructions[] = {
    0x2011, 0x6104, 0x2010, 0x2090,
};
static const struct pio_program qspi_out_program = {
    .instructions = qspi_out_program_instructions,
    .length = 4,
    .origin = -1,
    .pio_version = 0,
};

static inline pio_sm_config qspi_out_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + qspi_out_wrap_target, offset + qspi_out_wrap);
    return c;
}

#define qspi_in_wrap_target 1
#define qspi_in_wrap 3
static const uint16_t qspi_in_program_instructions[] = {
    0x2011, 0x2090, 0x4004, 0x2010,
};
static const struct pio_program qspi_in_program = {
    .instructions = qspi_in_program_instructions,
    .length = 4,
    .origin = -1,
    .pio_version = 0,
};

static inline pio_sm_config qspi_in_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + qspi_in_wrap_target, offset + qspi_in_wrap);
    return c;
}

static inline void qspi_out_program_init(PIO pio, uint sm, uint offset, uint pin)
{
    pio_sm_config c = qspi_out_program_get_default_config(offset);
    sm_config_set_out_pins(&c, pin, 4);
    pio_gpio_init(pio, pin);
    pio_gpio_init(pio, pin + 1);
    pio_gpio_init(pio, pin + 2);
    pio_gpio_init(pio, pin + 3);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_init(pio, sm, offset, &c);
}

static inline void qspi_out_status_program_init(PIO pio, uint sm, uint offset)
{
    pio_sm_config c = qspi_out_status_program_get_default_config(offset);
    pio_sm_init(pio, sm, offset, &c);
}

static inline void qspi_in_program_init(PIO pio, uint sm, uint offset, uint pin)
{
    pio_sm_config c = qspi_in_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, sm, offset, &c);
}

uint qspi_in_sm;
uint qspi_in_offset;
uint qspi_out_sm;
uint qspi_out_offset;
uint qspi_out_status_sm;
uint qspi_out_status_offset;

void __not_in_flash_func(qspi_init)(void)
{
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    qspi_out_offset = pio_add_program(pio1, &qspi_out_program);
    qspi_out_status_offset = pio_add_program(pio1, &qspi_out_status_program);
    qspi_in_offset = pio_add_program(pio1, &qspi_in_program);
    qspi_out_sm = pio_claim_unused_sm(pio1, true);
    qspi_out_status_sm = pio_claim_unused_sm(pio1, true);
    qspi_in_sm = pio_claim_unused_sm(pio1, true);

    printf("qspi_in_program offset %d\n", qspi_in_offset);
    printf("qspi_out_program offset %d\n", qspi_out_offset);
    printf("qspi_out_status_program offset %d\n", qspi_out_status_offset);

    qspi_out_program_init(pio1, qspi_out_sm, qspi_out_offset, 12);
    qspi_out_status_program_init(pio1, qspi_out_status_sm, qspi_out_status_offset);
    qspi_in_program_init(pio1, qspi_in_sm, qspi_in_offset, 12);
}
