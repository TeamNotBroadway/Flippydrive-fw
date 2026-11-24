cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(flippydrive C CXX ASM)
pico_sdk_init()

set(FD_VERSION 0.4.1)

# SDK settings that differ from PICO_BOARD=pico, read from the images: flash_range_erase/
# program assert against a 16 MB flash, and stdio_put_string takes no stdout mutex.
add_compile_definitions(PICO_FLASH_SIZE_BYTES=0x1000000 PICO_STDOUT_MUTEX=0)

add_subdirectory(third_party/fatfs)
add_subdirectory(third_party/esp-serial-flasher)
add_subdirectory(bootloader)
add_subdirectory(main-ode)
