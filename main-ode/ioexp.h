#pragma once
#include "pico/types.h"

void ioexp_read_output(void);
void ioexp_set_config(uint8_t config);
void ioexp_set_pin(uint pin, bool value);
