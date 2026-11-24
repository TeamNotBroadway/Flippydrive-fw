#!/bin/bash
# Build everything from source and check it against the original 1.4.6 release (run in WSL).
# Needs original/update.fpkg. Stops at the first image that is not identical; ends with each RP2040
# image's bytes by source (docs/EXACT_BUILD.md).
#
#   bash tools/check_all.sh [--skip-gc]     (--skip-gc reuses the GameCube outputs in ~/fd-build)
set -e
cd "$(dirname "$0")/.."
O=original/extracted
B=~/fd-build
LOG=$B/check_all.log
: > $LOG

same() {  # same BUILT ORIGINAL LABEL
    if cmp -s "$1" "$2"; then echo "identical  $3"; else echo "DIFFERS    $3 ($1 vs $2)"; exit 1; fi
}
step() { echo "--- $*" >> $LOG; "$@" >> $LOG 2>&1 || { echo "FAILED: $* (see $LOG)"; exit 1; }; }

step python3 -I tools/fpkg.py original/update.fpkg $O
step python3 -I tools/gcdisc.py $O/flippydrive-bootloader.bin $O/gc

if [ "$1" != --skip-gc ]; then
    step make -C gamecube/apploader
    step bash tools/match_gc.sh
    step make -C gamecube/payload
    step make -C gamecube/stub
    step make -C gamecube/disc
fi
same $B/gc-apploader/apploader.img $O/gc/apploader.img "GameCube apploader.img"
same $B/gc-payload/payload.dol $O/gc/payload.dol "GameCube payload.dol"
same $B/gc-stub/main.dol $O/gc/main.dol "GameCube main.dol"
# gcdisc.py's disc.iso runs past the disc's 0x258f4 bytes (the length word at 0x10025bd0).
head -c $((0x258f4)) $O/gc/disc.iso > $B/disc_orig.iso
same $B/gc-disc/disc.iso $B/disc_orig.iso "GameCube disc image"

step bash tools/wsl_build.sh . fw
for i in bootloader main-ode; do
    step python3 tools/rp_exact.py $i
    same $B/exact-$i/flippydrive-$i.bin $O/flippydrive-$i.bin "RP2040 $i (exact build)"
done

mkdir -p build
cp $B/exact-bootloader/flippydrive-bootloader.bin $B/exact-main-ode/flippydrive-main-ode.bin build/
step python3 -I tools/fpkg.py --pack original/update.fpkg build/update.fpkg \
    build/flippydrive-bootloader.bin build/flippydrive-main-ode.bin
same build/update.fpkg original/update.fpkg "update.fpkg"

echo
grep -A1 "functions from C" $LOG | grep -v "^--"
