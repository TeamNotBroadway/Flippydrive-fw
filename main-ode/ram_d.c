// RAM-resident DI commands: SEEK (0xAB), READ (0xA8) and WKF (0xDF). Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "ff.h"
#include "main_ode.h"
#include "log.h"
#include "ioexp.h"
#include "flippy.h"

extern uint di_sm;
extern uint32_t di_error_status;
extern uint8_t di_state;
extern int mount_h1;
extern FIL di_file;
extern uint8_t root_dir_res;
extern uint8_t net_stream_active;
extern bool disc_valid(int v);
extern bool disc_is_file(int v);
extern bool id_bitmap_test(uint32_t idx);
extern void di_stream_file(uint32_t len);
extern int di_stream_net(uint32_t addr, uint32_t len, uint sm, uint32_t fd);

int __not_in_flash_func(cmd_seek)(uint32_t a, uint32_t offset)
{
    uint32_t addr = offset << 2;
    LOG("DI", "SEEK 0x%08X\n", addr);
    if (di_state == 4) {
        di_error(DI_ERR_MOTOR_STOPPED);
        return 1;
    }
    pio_sm_put_blocking(pio0, di_sm, 0);
    (void)gpio_get_all();
    if (di_error_status != 0) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
    return 0;
}

int __not_in_flash_func(cmd_read)(uint32_t a, uint32_t offset, uint32_t len)
{
    len &= ~0x1fu;
    uint32_t addr = offset << 2;
    uint32_t fd = (a >> 16) & 0xff;
    LOG("DI", "READ 0x%08X len 0x%X fd %u\n", addr, len, fd);
    if (len != 0) {
        if (di_state == 4) {
            di_error(DI_ERR_MOTOR_STOPPED);
            return 1;
        }
        if (fd == 0)
            fd = mount_h1;
        if (!disc_valid(fd)) {
            LOG("DI", "READ INVALID FILE DESCRIPTOR %ul\n", fd);
            di_error(DI_ERR_BAD_HANDLE);
            return 1;
        }
        if (id_bitmap_test(fd))
            sleep_us(300);
        if (!disc_is_file(fd)) {
            net_stream_active = 0;
            root_dir_res = f_lseek(&di_file, addr);
            if (root_dir_res) {
                LOG("DI", "READ FLASH SEEK ERROR\n");
                di_error(DI_ERR_READ);
                return 1;
            }
            di_stream_file(len);
        } else {
            net_stream_active = 1;
            if (di_stream_net(addr, len, di_sm, fd)) {
                LOG("DI", "READ ERROR\n");
                di_error(DI_ERR_READ);
                return 1;
            }
        }
        if ((a & 0xc0) == 0x40)
            di_state = 0;
        if (di_error_status != 0) {
            ioexp_set_pin(2, true);
            di_error_status = 0;
        }
        return 0;
    }
    di_error(DI_ERR_BAD_HANDLE);
    return 1;
}

int __not_in_flash_func(cmd_wkf)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "WKF IGNORED\n");
    di_error(DI_ERR_BAD_HANDLE);
    return 0;
}
