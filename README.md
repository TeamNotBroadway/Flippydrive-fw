# flippydrive-fw

Matching decompilation of the FlippyDrive 1.4.6 release's RP2040 firmware and GameCube boot
code: human-readable C that builds to byte-identical images. The release's `update.fpkg`
(SHA-256 867a7fd8…1af0b1b) labels both RP2040 images version 0.4.1 in its entry headers.

## Status: work in progress, not usable

The GameCube images build byte-identical. The RP2040 images (bootloader and main-ode) build
and link but are not identical to the original, have never been run on hardware, and are
known not to work in full. Do not flash them.

Known broken or unverified (details in `docs/HACKS.md`):

- Bootloader ESP32 update: the port layer and update calls are decompiled
  (`bootloader/esp_port.c`), but the esp-serial-flasher library under them is the public
  source, not the original's modified copy, and `esp_loader_flash_start` is known to differ.
  A bad bootloader may not be recoverable.
- FatFs and esp-serial-flasher are the public sources; the original used modified copies,
  whose changes are not known yet.
- Functions that are not exact matches (bootloader 22 of 68, main-ode 30 of 95 scored
  functions) are checked for call order only. Conditions, constants and stores can still
  differ from the original.
- Some main-ode globals are unidentified, and their sizes are guessed.
- Functions are not at the original addresses.

## Targets

| Image | Load | Size | SHA-256 | Status |
|---|---|---|---|---|
| `flippydrive-bootloader.bin` | 0x10000000 | 0x40600 | fe5e5047…a0c64bf7e | links; 46 of 68 scored functions exact |
| `flippydrive-main-ode.bin` | 0x10080000 | 0x1b800 | cadba959…ec7be7f34 | links; 65 of 95 scored functions exact |
| GameCube disc image (bootloader 0x100002d0) | — | 0x258f4 | 1996f511…7e92af267 | identical (gc-boot-tools `create_iso_header`) |
| GameCube `main.dol` (in the bootloader's disc image) | 0x81100000 | 0x1d8f4 | 1b787f13…b08dc4c49 | identical |
| GameCube `apploader.img` (disc 0x2440) | 0x81200000 | 0x2394 | f9c16679…bad319f6b | identical (gc-boot-tools) |
| GameCube `payload.dol` (gzipped inside `main.dol`) | 0x80003100 | 0x462a0 | b855c27e…1413d76a4 | identical; application 55/55 functions C, 51 matching (the other 4 built from the original's code); constants and strings `.incbin`'d |

## Original package required

The build needs the 1.4.6 `update.fpkg` at `original/update.fpkg`. It is not in the
repository. The GameCube payload build takes these from it, so that vendor code and art are
not redistributed:

- the font and logo textures;
- the code of the four functions whose C does not match yet;
- the float constants and strings, which are `.incbin`'d.

The RP2040 build takes nothing from it directly; the bootloader embeds the GameCube disc.

`python -I tools/fpkg.py original/update.fpkg original/extracted` unpacks it and checks the
MD5s. Format notes are in `tools/fpkg.py`.
`python -I tools/gcdisc.py original/extracted/flippydrive-bootloader.bin
original/extracted/gc` extracts the GameCube disc and the payload's textures.

The build does not produce an `update.fpkg`: there is no packer yet.

## Build

Needs the tools listed in `docs/PLAN.md` (Arch gcc 14.2.0 and devkitPPC r47.1 under WSL Ubuntu,
pico-sdk 2.1.1, libogc2).

```
python -I tools/fpkg.py original/update.fpkg original/extracted
python -I tools/gcdisc.py original/extracted/flippydrive-bootloader.bin original/extracted/gc
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- make -C gamecube/apploader
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- bash tools/match_gc.sh
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- make -C gamecube/payload
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- make -C gamecube/stub
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- make -C gamecube/disc
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- bash tools/wsl_build.sh . fw
```

Each step takes the previous one's output; the bootloader includes the disc.

Output in WSL: `~/fd-build/fw/{bootloader,main-ode}/flippydrive-*.bin`,
`~/fd-build/gc-apploader/apploader.img`, `~/fd-build/gc-payload/payload.dol`,
`~/fd-build/gc-stub/main.dol` and `~/fd-build/gc-disc/disc.iso`.

See `docs/PLAN.md` for the toolchain evidence and the approach.
