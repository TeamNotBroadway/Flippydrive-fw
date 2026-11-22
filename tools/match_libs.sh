#!/bin/bash
# Name the library functions in both originals (run inside WSL after building ref/ and the
# firmware, tools/wsl_build.sh). Writes syms/<image>.lib.tsv: exact matches, then near
# matches for the vendored libraries the original modified (tools/nearmatch.py).
set -e
cd "$(dirname "$0")/.."
T=/mnt/c/Tools/arch-arm-none-eabi-14.2.0
N=$T/arm-none-eabi/lib/thumb/v6-m/nofp
F=~/fd-build/fw/third_party
VENDORED="$F/fatfs/libfatfs.a $F/esp-serial-flasher/libesp_serial_flasher.a"
LIBS="$N/libc.a $N/libm.a $N/libnosys.a $T/lib/gcc/arm-none-eabi/14.2.0/thumb/v6-m/nofp/libgcc.a"
mkdir -p syms
for t in bootloader:0x10000000 main-ode:0x10080000; do
    n=${t%%:*}; b=${t#*:}
    python3 -I tools/symmatch.py original/extracted/flippydrive-$n.bin $b syms/$n.lib.tsv \
        ~/fd-build/ref $LIBS $VENDORED "$@"
    python3 -I tools/nearmatch.py original/extracted/flippydrive-$n.bin $b syms/$n.lib.tsv $VENDORED
done
