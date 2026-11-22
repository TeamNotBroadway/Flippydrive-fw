// The I2C port expander at 0x20 (output register 1, configuration register 3). Names are ours.
#include "ioexp.h"
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "hardware/i2c.h"

#define IOEXP_ADDR 0x20
#define IOEXP_REG_OUTPUT 1
#define IOEXP_REG_CONFIG 3

auto_init_mutex(ioexp_mutex);
static uint8_t ioexp_output;

void ioexp_read_output(void)
{
    uint8_t reg = IOEXP_REG_OUTPUT;
    mutex_enter_blocking(&ioexp_mutex);
    i2c_write_blocking(i2c1, IOEXP_ADDR, &reg, 1, true);
    i2c_read_blocking(i2c1, IOEXP_ADDR, &ioexp_output, 1, false);
    mutex_exit(&ioexp_mutex);
}

void ioexp_set_config(uint8_t config)
{
    mutex_enter_blocking(&ioexp_mutex);
    uint8_t buf[2] = {IOEXP_REG_CONFIG, config};
    i2c_write_blocking(i2c1, IOEXP_ADDR, buf, 2, false);
    mutex_exit(&ioexp_mutex);
}

void ioexp_set_pin(uint pin, bool value)
{
    mutex_enter_blocking(&ioexp_mutex);
    uint8_t mask = 1u << pin;
    if (value)
        ioexp_output = ioexp_output | mask;
    else
        ioexp_output = ioexp_output & (uint8_t)~mask;
    uint8_t buf[2] = {IOEXP_REG_OUTPUT, ioexp_output};
    i2c_write_blocking(i2c1, IOEXP_ADDR, buf, 2, false);
    mutex_exit(&ioexp_mutex);
}
