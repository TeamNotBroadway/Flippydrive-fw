# Reference build: plain pico-sdk 2.1.1 code compiled with the firmware's toolchain and the
# originals' settings, for matching library functions in the original images.
# Build with: tools/wsl_build.sh ref ref
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(ref C CXX ASM)
pico_sdk_init()
add_executable(ref main.c)
target_link_libraries(ref pico_stdlib pico_multicore pico_flash pico_unique_id pico_rand
    pico_i2c_slave pico_aon_timer pico_async_context_threadsafe_background pico_bootrom
    hardware_adc hardware_clocks hardware_dma hardware_exception hardware_flash
    hardware_i2c hardware_interp hardware_irq hardware_pio hardware_pll hardware_pwm
    hardware_resets hardware_rtc hardware_spi hardware_sync hardware_timer hardware_uart
    hardware_vreg hardware_watchdog hardware_xosc)
pico_enable_stdio_usb(ref 1)
pico_enable_stdio_uart(ref 0)
