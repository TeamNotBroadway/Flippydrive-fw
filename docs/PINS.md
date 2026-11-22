# RP2040 pin use (main-ode)

From main-ode's GPIO setup (`main` 0x10080424, bus release 0x100801e8) and its PIO programs
(structs at 0x10093cf8-0x10093d94). Signal names are inferred from what the programs do and
the GameCube drive interface's signal set; none is labelled in the firmware. "Confirmed"
means a PIO program or register write shows the role directly.

| GPIO | Setup | Role | Basis |
|---|---|---|---|
| 0-7 | PIO0; pads pull-up + pull-down (keeper), Schmitt, fast slew | DI data DID0-7 | confirmed: `in pins 8` / `out pins 8` in the DI programs |
| 25 | input | DIHSTRB (host strobe) | confirmed: command bytes latched on its rising edge |
| 27 | input, falling-edge IRQ | DIDIR (direction) | command receive waits for it low, reply for it high |
| 26 | input, pull-up | DIRST (reset) | every DI program first waits for it high |
| 28 | output high, switched to PIO0 during replies | DIDSTRB (drive strobe) | inferred: driven idle-high by SIO, handed to PIO for transfers |
| 29 | output low | DICOVER or DIERRB | inferred only |
| 21 | input (released) | DIBRK or DIERRB | inferred only; firmware logs "BRK received" |
| 22 | input | AISLR (audio L/R clock) | confirmed: `ai_program` (PIO1) syncs on its edges, 32 bits per frame |
| 24 | input | AISCLK (audio bit clock) | confirmed: one `out pins 1` per edge |
| 23 | output low | AISD (audio data), probably | `ai_program`'s out pin is set in its SM config, not yet read |
| 12-15 | PIO1 | 4-bit link data | confirmed: `qspi_in/out_program` (`out_base 12`, count 4) |
| 16 | input | link clock | confirmed: QSPI programs clock on it |
| 17 | input, pull-up | link select | confirmed: QSPI programs wait for it low |
| 18, 19 | I2C1 SDA/SCL, pull-ups | I2C | `gpio_set_function(18/19, GPIO_FUNC_I2C)` |
| 20 | output high | unknown | |

The QSPI link is probably the ESP32 (it is not on the DI bus and the bootloader talks to the
ESP32); unverified.
