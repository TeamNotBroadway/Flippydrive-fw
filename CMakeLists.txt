cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(flippydrive C CXX ASM)
pico_sdk_init()

set(FD_VERSION 0.4.1)

add_subdirectory(third_party/fatfs)
add_subdirectory(third_party/esp-serial-flasher)
add_subdirectory(bootloader)
add_subdirectory(main-ode)
