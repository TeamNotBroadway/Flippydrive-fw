# esp-serial-flasher at 3937d49, unmodified. The original is a modified fork near this
# commit (docs/PLAN.md); the loader_port_* functions are application code.
add_library(esp_serial_flasher STATIC
    src/esp_loader.c src/esp_targets.c src/md5_hash.c src/serial_comm.c)
target_include_directories(esp_serial_flasher PUBLIC include PRIVATE private_include)
target_compile_definitions(esp_serial_flasher PUBLIC MD5_ENABLED=0)
target_compile_options(esp_serial_flasher PRIVATE -ffunction-sections -fdata-sections)
