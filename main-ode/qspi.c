// The QSPI link's PIO1 state machines. Names other than the programs' are ours.
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/structs/busctrl.h"
#include "log.h"
#include "qspi.pio.h"

#define QSPI_PIN_D0 12

uint qspi_out_offset;
uint qspi_out_status_offset;
uint qspi_in_offset;
uint qspi_out_sm;
uint qspi_out_status_sm;
uint qspi_in_sm;

void qspi_init(void)
{
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    qspi_out_offset = pio_add_program(pio1, &qspi_out_program);
    qspi_out_status_offset = pio_add_program(pio1, &qspi_out_status_program);
    qspi_in_offset = pio_add_program(pio1, &qspi_in_program);
    qspi_out_sm = pio_claim_unused_sm(pio1, true);
    qspi_out_status_sm = pio_claim_unused_sm(pio1, true);
    qspi_in_sm = pio_claim_unused_sm(pio1, true);

    LOG("QSPI", "qspi_in_program offset %d\n", qspi_in_offset);
    LOG("QSPI", "qspi_out_program offset %d\n", qspi_out_offset);
    LOG("QSPI", "qspi_out_status_program offset %d\n", qspi_out_status_offset);

    {
        uint sm = qspi_out_sm;
        uint off = qspi_out_offset;
        pio_sm_config c = qspi_out_program_get_default_config(off);
        sm_config_set_out_pins(&c, QSPI_PIN_D0, 4);
        pio_gpio_init(pio1, QSPI_PIN_D0);
        pio_gpio_init(pio1, QSPI_PIN_D0 + 1);
        pio_gpio_init(pio1, QSPI_PIN_D0 + 2);
        pio_gpio_init(pio1, QSPI_PIN_D0 + 3);
        sm_config_set_out_shift(&c, false, true, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        pio_sm_init(pio1, sm, off, &c);
    }
    {
        uint sm = qspi_out_status_sm;
        uint off = qspi_out_status_offset;
        pio_sm_config c = qspi_out_status_program_get_default_config(off);
        pio_sm_init(pio1, sm, off, &c);
    }
    {
        uint sm = qspi_in_sm;
        uint off = qspi_in_offset;
        pio_sm_config c = qspi_in_program_get_default_config(off);
        sm_config_set_in_pins(&c, QSPI_PIN_D0);
        sm_config_set_in_shift(&c, false, true, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        pio_sm_init(pio1, sm, off, &c);
    }
}
