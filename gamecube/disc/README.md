# GameCube disc

From OffBroadway/gc-boot-tools 8d59fb2 (branch save-ipl-fastboot, GPLv2, `COPYING`), the
revision `gamecube/apploader` comes from: `create_iso_header/create_iso_header.c`,
`include/gcm.h` and `icons/opening.bnr` (equal to the disc's).

Edit: `create_iso_header.c` zeroes its header buffer, which upstream leaves uninitialised.
The Makefile copies the original's bytes for the one range where that showed
(`docs/HACKS.md`).
