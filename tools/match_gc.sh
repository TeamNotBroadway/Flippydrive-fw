#!/bin/bash
# Name the library functions in the GameCube payload (run inside WSL after tools/gcdisc.py).
# Builds libogc2 (C:\Tools\libogc2, checked out at 70f80d8) for GameCube with devkitPPC r47.1
# into ~/fd-build/libogc2, then matches it, newlib, libsysbase, libgcc and the crt objects
# against payload.dol. Writes syms/gc-payload.lib.tsv.
set -e
cd "$(dirname "$0")/.."
export DEVKITPRO=${DEVKITPRO:-/mnt/c/Tools/devkitppc-20250727/root/opt/devkitpro}
export DEVKITPPC=$DEVKITPRO/devkitPPC PATH=$DEVKITPRO/devkitPPC/bin:$PATH
O=${OGC_BUILD:-~/fd-build/libogc2-70f80d8}
if [ ! -f $O/lib/cube/libogc.a ]; then
    rm -rf $O && git clone -q --shared /mnt/c/Tools/libogc2 $O
    git -C $O checkout -q 70f80d8
    make -C $O -j16 cube > $O/build.log 2>&1
fi
CC="powerpc-eabi-gcc -mogc -mcpu=750 -meabi -mhard-float"
L=$DEVKITPPC/powerpc-eabi/lib/$($CC -print-multi-directory)
G=$(dirname $($CC -print-libgcc-file-name))
mkdir -p syms
python3 -I tools/symmatch.py original/extracted/gc/payload.dol 0 syms/gc-payload.lib.tsv \
    $O/lib/cube/*.a $O/cube/*asm*.o $O/cube/ogc_crt0.o $L/libc.a $L/libm.a $L/libsysbase.a \
    $L/crtmain.o $G/libgcc.a $G/crtbegin.o $G/crtend.o $G/ecrti.o $G/ecrtn.o "$@"
