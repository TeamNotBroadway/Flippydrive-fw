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

    qspi_out_program_init(pio1, qspi_out_sm, qspi_out_offset, QSPI_PIN_D0);
    qspi_out_status_program_init(pio1, qspi_out_status_sm, qspi_out_status_offset);
    qspi_in_program_init(pio1, qspi_in_sm, qspi_in_offset, QSPI_PIN_D0);
}
