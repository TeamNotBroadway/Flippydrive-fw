#!/bin/bash
# Configure and build a CMake project under WSL with the firmware's toolchain.
# Usage (inside WSL): tools/wsl_build.sh SOURCE_DIR BUILD_NAME
# The build tree goes in ~/fd-build/BUILD_NAME (WSL's own filesystem; /mnt is slow).
set -e
SRC=$(realpath "$1"); B=~/fd-build/$2
export PICO_SDK_PATH=/mnt/c/Tools/pico-sdk-2.1.1
export PICO_TOOLCHAIN_PATH=/mnt/c/Tools/arch-arm-none-eabi-14.2.0
# Arch binutils objdump/readelf need libdebuginfod.so.1, taken from Ubuntu's libdebuginfod1t64.
export LD_LIBRARY_PATH=/mnt/c/Tools/arch-arm-none-eabi-14.2.0/hostlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# __DATE__ in the originals is "Jun  6 2025"; gcc takes it from SOURCE_DATE_EPOCH.
export SOURCE_DATE_EPOCH=1749211200
export PATH=/mnt/c/Tools/cmake-3.31.8-linux-x86_64/bin:/mnt/c/Tools/ninja-linux-1.12.1:$PATH
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico
cmake --build "$B"
