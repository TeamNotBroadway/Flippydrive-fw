// Core 0: board setup, then the log printer and housekeeping loop. Names are ours.
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/mutex.h"
#include "hardware/i2c.h"
#include "hardware/structs/padsbank0.h"
#include "ioexp.h"
#include "main_ode.h"
#include "log.h"
#include "image_header.h"

IMAGE_HEADER("flippydrive-main-ode", FD_VERSION);

#define CORE1_STACK_SIZE 0x2800

queue_t log_queue;
mutex_t flash_mutex;
mutex_t aud_mutex;
volatile int aud_enabled;

// Near match: GCC lays out the log-drain loop without the original's duplicated 8-byte
// block on the success path (docs/HACKS.md).
int main(void)
{
    log_init();
    stdio_usb_init();

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

    ioexp_read_output();
    ioexp_set_pin(7, true);
    ioexp_set_pin(6, true);
    ioexp_set_pin(2, true);
    ioexp_set_pin(4, false);
    ioexp_set_pin(1, true);
    ioexp_set_pin(0, true);
    ioexp_set_config(8);

    multicore_lockout_victim_init();
    puts("Core 0: Init");
    queue_init(&log_queue, 4, 10);
    mutex_init(&flash_mutex);
    mutex_init(&bus_mutex);
    qspi_init();
    mount_fat();
    puts("Core 0: Starting DI Worker...");
    mutex_init(&aud_mutex);
    multicore_launch_core1_with_stack(di_worker, malloc(CORE1_STACK_SIZE), CORE1_STACK_SIZE);
    puts("Core 0: Entering event loop...");

    for (;;) {
        while (!queue_is_empty(&log_queue)) {
            char *msg;
            if (queue_try_remove(&log_queue, &msg)) {
                fputs(msg, stdout);
                log_free(msg);
                continue; // no-op; GCC's continue predictor gives the original block layout
            }
        }
        flush_task();
        if (aud_enabled) {
            mutex_enter_blocking(&aud_mutex);
            audio_poll();
            mutex_exit(&aud_mutex);
        }
        for (;;) {
            mutex_enter_blocking(&bus_mutex);
            if (!bus_released)
                break;
            ioexp_set_pin(4, gpio_get(26));
            mutex_exit(&bus_mutex);
        }
        mutex_exit(&bus_mutex);
    }
}

int (*cmd_handlers[256])(void);

void set_cmd_handler(int cmd, int (*handler)(void))
{
    cmd_handlers[cmd] = handler;
}
