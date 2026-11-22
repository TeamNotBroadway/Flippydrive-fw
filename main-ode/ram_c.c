// RAM-resident DI GPIO IRQ handler and bypass-mode entry. Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "log.h"
#include "ioexp.h"
#include "main_ode.h"

#define DI_PIN_DSTRB 28
#define DI_PIN_DIR 27
#define DI_PIN_BRK 21
#define DI_PIN_RESET 26

extern uint di_sm;
extern uint di_offset;
extern uint di_dir_sm_lo;
extern uint di_dir_sm_hi;
extern semaphore_t di_sem;
extern uint8_t di_state;
extern void di_setup(void);
extern void bus_acquire(void);
extern void audio_reset(void);
extern void audio_dma_deinit(void);
extern void bypass_wait(void);

// Raw instructions: the original's bypass program, not decoded.
static const uint16_t bypass_instr[8] = {0xe02b, 0x209a, 0x201b, 0x2019, 0x2099, 0x4008, 0x0043, 0x209b};
const struct pio_program bypass_program = {.instructions = bypass_instr, .length = 8, .origin = -1};

static inline pio_sm_config bypass_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 7);
    return c;
}

static inline void di_sm_stop(void)
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

// DIR falling: restart the state machine. BRK rising: reset the bus. RESET level: drive reset.
void __not_in_flash_func(di_gpio_irq)(uint gpio, uint32_t events)
{
    if (gpio == DI_PIN_DIR && (events & GPIO_IRQ_EDGE_FALL)) {
        di_sm_stop();
        sem_init(&di_sem, 0, 1);
        di_sm_start();
        gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, false);
    }
    if (gpio == DI_PIN_BRK && (events & GPIO_IRQ_EDGE_RISE)) {
        di_sm_stop();
        sem_init(&di_sem, 0, 1);
        gpio_put(DI_PIN_BRK, 0);
        gpio_set_dir(DI_PIN_BRK, GPIO_OUT);
        LOG("DI", "BRK received\n");
        busy_wait_ms(50);
        gpio_set_dir(DI_PIN_BRK, GPIO_IN);
        di_sm_start();
    }
    if (gpio == DI_PIN_RESET) {
        if (events & GPIO_IRQ_LEVEL_LOW) {
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW, false);
            di_sm_stop();
            sem_init(&di_sem, 0, 1);
            di_state = 5;
            audio_reset();
            di_sm_start();
            LOG("DI", "Reset drive\n");
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_HIGH, true);
        } else if (events & GPIO_IRQ_LEVEL_HIGH) {
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_HIGH, false);
            gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW, true);
        }
    }
    gpio_acknowledge_irq(gpio, events);
}

// Tear down the DI state machines and run the bypass program until bypass_wait sees the exit
// command, then bring DI back up.
int __not_in_flash_func(bypass_enter)(void)
{
    pio_sm_put_blocking(pio0, di_sm, 0);
    LOG("DI", "Entering Bypass\n");
    bus_release();
    pio_sm_set_enabled(pio0, di_sm, false);
    pio_sm_drain_tx_fifo(pio0, di_sm);
    pio_sm_clear_fifos(pio0, di_sm);
    pio_sm_restart(pio0, di_sm);
    sem_init(&di_sem, 0, 1);
    gpio_set_irq_enabled(DI_PIN_BRK, GPIO_IRQ_EDGE_RISE, false);
    gpio_set_irq_enabled(DI_PIN_RESET, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH, false);
    gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, false);
    irq_set_enabled(IO_IRQ_BANK0, false);
    pio_sm_set_enabled(pio0, di_dir_sm_lo, false);
    pio_sm_set_enabled(pio0, di_dir_sm_hi, false);
    audio_reset();
    audio_dma_deinit();
    pio_clear_instruction_memory(pio0);
    pio_sm_unclaim(pio0, di_sm);
    pio_sm_unclaim(pio0, di_dir_sm_lo);
    pio_sm_unclaim(pio0, di_dir_sm_hi);
    di_offset = pio_add_program(pio0, &bypass_program);
    uint sm = pio_claim_unused_sm(pio0, true);
    uint off = di_offset;
    di_sm = sm;
    pio_sm_config c = bypass_program_get_default_config(off);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio0, sm, off, &c);
    pio_sm_set_enabled(pio0, sm, true);
    bypass_wait();
    pio_sm_set_enabled(pio0, di_sm, false);
    pio_clear_instruction_memory(pio0);
    pio_sm_unclaim(pio0, di_sm);
    bus_acquire();
    di_setup();
    ioexp_set_pin(6, false);
    return 0;
}
