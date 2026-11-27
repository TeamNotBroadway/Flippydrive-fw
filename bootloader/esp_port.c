// esp-serial-flasher port layer and the ESP32 update calls built on it (originals at
// 0x2000968c-0x2000998e). The ESP32's EN and GPIO0 are on the I/O expander. Names are ours.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "esp_loader.h"
#include "serial_io.h"
#include "bootloader.h"

void expander_set(uint8_t pin, bool on);
esp_loader_error_t loader_sync_cmd(void);

typedef struct {
    uart_inst_t *uart;
    uint32_t tx_pin;
    uint32_t rx_pin;
    uint32_t pin3, pin4;    // 18 and 19; not read
    uint32_t reset_pin;     // expander pins
    uint32_t boot_pin;
} esp_port_config_t;

static uart_inst_t *s_uart;
static uint32_t s_tx_pin, s_rx_pin, s_boot_pin, s_reset_pin;
static absolute_time_t s_time_end;

void loader_port_enter_bootloader(void)
{
    expander_set(s_boot_pin, false);
    expander_set(s_reset_pin, false);
    busy_wait_ms(5);
    expander_set(s_reset_pin, true);
    busy_wait_ms(2);
    expander_set(s_boot_pin, true);
}

void loader_port_reset_target(void)
{
    expander_set(s_reset_pin, false);
    busy_wait_ms(5);
    expander_set(s_reset_pin, true);
}

void loader_port_delay_ms(uint32_t ms)
{
    busy_wait_ms(ms);
}

esp_loader_error_t loader_port_serial_write(const uint8_t *data, uint16_t size, uint32_t timeout)
{
    uart_write_blocking(s_uart, data, size);
    uart_tx_wait_blocking(s_uart);
    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t loader_port_serial_read(uint8_t *data, uint16_t size, uint32_t timeout)
{
    memset(data, 0, size);
    if (!uart_is_readable_within_us(s_uart, timeout * 1000))
        return ESP_LOADER_ERROR_TIMEOUT;
    uart_read_blocking(s_uart, data, size);
    return ESP_LOADER_SUCCESS;
}

void esp_port_init(const esp_port_config_t *config)
{
    s_uart = config->uart;
    s_tx_pin = config->tx_pin;
    s_rx_pin = config->rx_pin;
    s_boot_pin = config->boot_pin;
    s_reset_pin = config->reset_pin;
    expander_set(s_reset_pin, false);
    expander_set(s_boot_pin, true);
    busy_wait_ms(10);
    expander_set(s_reset_pin, true);
    busy_wait_ms(10);
    uart_init(s_uart, 115200);
    gpio_set_function(s_tx_pin, GPIO_FUNC_UART);
    gpio_set_function(s_rx_pin, GPIO_FUNC_UART);
}

void loader_port_start_timer(uint32_t ms)
{
    s_time_end = make_timeout_time_ms(ms);
}

uint32_t loader_port_remaining_time(void)
{
    int32_t remaining = absolute_time_diff_us(get_absolute_time(), s_time_end) / 1000;
    return remaining > 0 ? remaining : 0;
}

void loader_port_debug_print(const char *str)
{
    printf("DEBUG: %s\n", str);
}

// Inlined into connect_to_target; the image has no separate copy.
esp_loader_error_t loader_port_change_baudrate(uint32_t baudrate)
{
    uint current = uart_set_baudrate(uart0, baudrate);
    if (current != baudrate) {
        printf("current_baud = %d\n", current);
        return ESP_LOADER_ERROR_FAIL;
    }
    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t connect_to_target(uint32_t higher_baudrate)
{
    esp_port_config_t config = {uart0, 16, 17, 18, 19, 5, 0};
    esp_port_init(&config);
    esp_loader_connect_args_t connect_config = {.sync_timeout = 10, .trials = 20};
    esp_loader_error_t err = esp_loader_connect(&connect_config);
    if (unlikely(err != ESP_LOADER_SUCCESS)) {
        printf("Cannot connect to target. Error: %u\n", err);
        return err;
    }
    if (higher_baudrate != 115200 && esp_loader_get_target() != ESP8266_CHIP) {
        err = esp_loader_change_baudrate(higher_baudrate);
        if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
            printf("ESP8266 does not support change baudrate command.\n");
            return err;
        } else if (err != ESP_LOADER_SUCCESS) {
            printf("Unable to change baud rate on target.\n");
            return err;
        } else {
            err = loader_port_change_baudrate(higher_baudrate);
            if (err != ESP_LOADER_SUCCESS) {
                printf("Unable to change baud rate.\n");
                return err;
            }
            printf("Baudrate changed\n");
        }
    }
    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t esp_flash_start(uint32_t addr, uint32_t len, uint32_t block)
{
    printf("Erasing flash (this may take a while)...\n");
    esp_loader_error_t err = esp_loader_flash_start(addr, len, block);
    if (err != ESP_LOADER_SUCCESS)
        printf("Erasing flash failed with error %d.\n", err);
    return err;
}

esp_loader_error_t esp_flash_write(uint32_t addr, void *data, uint32_t len)
{
    esp_loader_error_t err = esp_loader_flash_write(data, len);
    if (err != ESP_LOADER_SUCCESS)
        printf("\nESP32 Block write fail 0x%x - %d.\n", addr, err);
    return err;
}

// Called after the last block. The original sends SYNC here, not FLASH_END.
void esp_flash_finish(void)
{
    loader_sync_cmd();
}
