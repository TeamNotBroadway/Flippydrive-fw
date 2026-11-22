#include <stdio.h>
#include "pico/stdlib.h"

int main(void)
{
    stdio_init_all();
    for (;;) {
        printf("%f\n", 1.5);
        sleep_ms(1000);
    }
}
