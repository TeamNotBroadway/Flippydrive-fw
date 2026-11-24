add_executable(flippydrive-bootloader main.c app_a.c app_b.c app_c.c app_d.c esp_port.c flash_blob.S)
# The GameCube disc, built by gamecube/disc.
set(GC_DISC "$ENV{HOME}/fd-build/gc-disc/disc.iso" CACHE FILEPATH "GameCube disc image")
set_source_files_properties(flash_blob.S PROPERTIES OBJECT_DEPENDS "${GC_DISC}")
target_include_directories(flippydrive-bootloader PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_compile_definitions(flippydrive-bootloader PRIVATE FD_VERSION="${FD_VERSION}"
    DISC_FILE="${GC_DISC}")
# Diagnostic ESP32 extraction build (docs/ESP32.md). OFF by default so the build stays
# byte-identical to the original; -DFD_ESP_EXTRACT=ON adds the clean-bridge (0x11) command.
option(FD_ESP_EXTRACT "Add the ESP32 clean-bridge diagnostic command" OFF)
if(FD_ESP_EXTRACT)
    target_compile_definitions(flippydrive-bootloader PRIVATE FD_ESP_EXTRACT)
endif()
target_link_libraries(flippydrive-bootloader pico_stdlib pico_multicore hardware_i2c hardware_pio hardware_dma hardware_flash fatfs esp_serial_flasher)
pico_set_binary_type(flippydrive-bootloader copy_to_ram)
pico_set_linker_script(flippydrive-bootloader ${CMAKE_CURRENT_SOURCE_DIR}/memmap_bootloader.ld)
pico_set_program_name(flippydrive-bootloader flippydrive-bootloader)
pico_set_program_version(flippydrive-bootloader ${FD_VERSION})
pico_enable_stdio_usb(flippydrive-bootloader 1)
pico_enable_stdio_uart(flippydrive-bootloader 0)
pico_add_bin_output(flippydrive-bootloader)
pico_add_map_output(flippydrive-bootloader)
