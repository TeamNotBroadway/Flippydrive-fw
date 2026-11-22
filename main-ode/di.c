// The drive-interface side of core 0. Names are ours.
#include "pico/stdlib.h"
#include "hardware/structs/padsbank0.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "log.h"
#include "ioexp.h"
#include "main_ode.h"

#define DI_PIN_DSTRB 28
#define DI_PIN_DIR 27

uint32_t di_error_status;
uint di_sm;
uint di_offset;
semaphore_t di_sem;

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

// Report an error to the host and restart the DI state machine.
void di_error(uint32_t status)
{
    di_error_status = status;
    LOG("DI", "ERROR in-band 0x%08X\n", status);
    gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, false);
    di_sm_stop();
    sem_init(&di_sem, 0, 1);
    ioexp_set_pin(2, false);
    gpio_set_irq_enabled(DI_PIN_DIR, GPIO_IRQ_EDGE_FALL, true);
    di_sm_start();
}

// Size not known; only its use as a DMA target at an offset is seen.
uint8_t di_buffer[0x4000];
int di_dma_chan = -1;

// Receive len bytes from the host into di_buffer + offset (DMA from the DI state machine
// RX FIFO, byte-swapped); the DMA IRQ handler releases di_sem.
void di_receive(uint32_t len, uint32_t offset)
{
    di_sm_stop();
    sem_init(&di_sem, 0, 1);

    di_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(di_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio0, di_sm, false));
    channel_config_set_bswap(&c, true);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(di_dma_chan, &c, di_buffer + offset, &pio0->rxf[di_sm], len >> 2, false);
    dma_channel_set_irq0_enabled(di_dma_chan, true);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(di_dma_chan);

    pio_sm_put_blocking(pio0, di_sm, len - 4);
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_pull(false, true) | pio_encode_sideset(1, 1));
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_mov(pio_x, pio_osr) | pio_encode_sideset(1, 1));
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_set(pio_y, 2) | pio_encode_sideset(1, 1));
    pio_sm_exec_wait_blocking(pio0, di_sm, pio_encode_jmp(di_offset) | pio_encode_sideset(1, 1));
    gpio_set_function(DI_PIN_DSTRB, GPIO_FUNC_PIO0);
    pio_sm_set_enabled(pio0, di_sm, true);

    sem_acquire_blocking(&di_sem);

    di_sm_stop();
    sem_init(&di_sem, 0, 1);
    di_sm_start();
}

mutex_t bus_mutex;
volatile bool bus_released;

// Stop driving the DI bus: every bus pin back to an SIO input.
void bus_release(void)
{
    mutex_enter_blocking(&bus_mutex);
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
    gpio_set_dir(25, GPIO_IN);
    gpio_set_dir(21, GPIO_IN);
    gpio_set_dir(23, GPIO_IN);
    gpio_set_dir(22, GPIO_IN);
    gpio_set_dir(24, GPIO_IN);
    gpio_set_dir(29, GPIO_IN);
    gpio_set_dir(25, GPIO_IN);
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
    gpio_init(26);
    gpio_set_dir(26, GPIO_IN);
    gpio_pull_up(26);
    ioexp_set_pin(1, false);
    ioexp_set_config(0x5c);
    bus_released = true;
    mutex_exit(&bus_mutex);
}

void bus_acquire(void)
{
    mutex_enter_blocking(&bus_mutex);
    bus_released = false;
    ioexp_set_pin(4, false);
    gpio_init(23);
    gpio_put(23, 0);
    gpio_set_dir(23, GPIO_OUT);
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
    gpio_init(25);
    gpio_set_dir(25, GPIO_IN);
    gpio_init(21);
    gpio_set_dir(21, GPIO_IN);
    gpio_init(29);
    gpio_put(29, 0);
    gpio_set_dir(29, GPIO_OUT);
    gpio_init(26);
    gpio_set_dir(26, GPIO_IN);
    gpio_pull_up(26);
    ioexp_set_pin(6, true);
    ioexp_set_pin(2, true);
    ioexp_set_pin(1, true);
    ioexp_set_config(8);
    mutex_exit(&bus_mutex);
}

void di_init_handlers(void)
{
    set_cmd_handler(DI_CMD_REQUEST_ERROR, cmd_request_error);
}

REGISTER_CMD(DI_CMD_IPC, cmd_ipc);
REGISTER_CMD(DI_CMD_BYPASS, bypass_enter);
REGISTER_CMD(DI_CMD_WKF, cmd_wkf);
REGISTER_CMD(DI_CMD_READ, cmd_read);
REGISTER_CMD(DI_CMD_SEEK, cmd_seek);
REGISTER_CMD(DI_CMD_INQUIRY, cmd_inquire);
REGISTER_CMD(DI_CMD_STOP_MOTOR, cmd_motor_stop);
PICO_RUNTIME_INIT_FUNC_RUNTIME(di_init_handlers, "99999");
