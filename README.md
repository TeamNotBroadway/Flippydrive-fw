# flippydrive-fw

Matching decompilation of the FlippyDrive 1.4.6 release's RP2040 firmware and GameCube boot
code: human-readable C that builds to byte-identical images. The release's `update.fpkg`
(SHA-256 867a7fd8…1af0b1b) labels both RP2040 images version 0.4.1 in its entry headers.

## Status: work in progress, not usable

The GameCube images build byte-identical. The RP2040 images have two builds
(`docs/EXACT_BUILD.md`): the exact build is byte-identical, with the functions whose C does not
match yet taken from the original image; the C build has every function from C, is not
identical, has never been run on hardware, and is known not to work in full. Do not flash the
C build.

The known problems below are in the C build.

Known broken or unverified (details in `docs/HACKS.md`):

- Bootloader ESP32 update: the port layer and update calls are decompiled
  (`bootloader/esp_port.c`), and esp-serial-flasher's two fork-modified functions
  (`esp_loader_connect`, `esp_loader_flash_start`) are now matched in the vendored source
  (`docs/HACKS.md`). Untested on hardware; a bad bootloader may not be recoverable.
- Functions that are not byte-identical may behave differently from the original (Decompilation
  status below lists them and how far each is checked).
- Some main-ode globals are unidentified, and their sizes are guessed.
- Functions are not at the original addresses.

## Decompilation status

Every function falls into one of three groups. The tables are generated: run
`tools/check_all.sh`, then `python3 tools/exact_summary.py` and `python3 tools/equiv.py --md`.

1. **Byte-identical.** Every section the exact build links from our objects compiles to the
   original's bytes at the original's address. This covers all FatFs, pico-sdk, TinyUSB,
   esp-serial-flasher, newlib and libgcc code located in the images, and every application
   function not in the last table below.
2. **Same instructions, different registers or order.** Not byte-identical, but the
   instructions match the original's once register names and branch targets are erased
   (`register-only`), or match as a set in a different order or block layout (`reordered`).
   Very likely to behave the same; not proven.
3. **May differ in behaviour.** Anything else (`differs`). Hand inspection found register
   choice and block layout as the remaining differences in most of these, but the check cannot
   show that, so they are unverified.

Bytes in each image by source (exact build):

**bootloader** (0x40600 bytes; 59 of 67 `src.tsv` application functions from C)

| Component | Bytes from C | Share of image |
|---|---:|---:|
| Application (decompiled) | 8110 | 3.1% |
| FatFs R0.15 (vendor-edited) | 27832 | 10.6% |
| esp-serial-flasher (fork-edited) | 4230 | 1.6% |
| pico-sdk 2.1.1 | 26260 | 10.0% |
| TinyUSB (SDK 2.1.1) | 11325 | 4.3% |
| newlib / libgcc | 23441 | 8.9% |
| Embedded GameCube disc and data | 153860 | 58.4% |
| Strings, padding and fill (generated) | 5010 | 1.9% |
| **Copied from the original** | **3612** | **1.4%** |

**main-ode** (0x1b800 bytes; 47 of 55 `src.tsv` application functions from C)

| Component | Bytes from C | Share of image |
|---|---:|---:|
| Application (decompiled) | 15486 | 13.7% |
| FatFs R0.15 (vendor-edited) | 27484 | 24.4% |
| pico-sdk 2.1.1 | 26908 | 23.9% |
| TinyUSB (SDK 2.1.1) | 11325 | 10.1% |
| newlib / libgcc | 8985 | 8.0% |
| Strings, padding and fill (generated) | 7407 | 6.6% |
| **Copied from the original** | **15045** | **13.4%** |

Functions that are not byte-identical (the exact build copies their bytes from the original):

| Image | Function | Address | Original bytes | Instructions ours/orig | Class | Match | Calls | Literals |
|---|---|---|---:|---:|---|---:|---|---|
| bootloader | `usb_uart_bridge` | 0x200000e4 | 256 | 107/107 | reordered | 100% | same | same |
| bootloader | `fetch_update` | 0x20000424 | 648 | 267/273 | differs | 96% | same | differs (ours: one section address) |
| bootloader | `update_and_boot` | 0x200006ac | 852 | 330/334 | differs | 96% | differs (4) | same |
| bootloader | `install_update` | 0x20000fac | 612 | 219/215 | differs | 97% | same | same |
| bootloader | `flash_esp32` | 0x200013a0 | 344 | 149/147 | differs | 97% | same | same |
| bootloader | `flash_rp2040` | 0x200014f8 | 312 | 135/135 | differs | 98% | differs (2) | same |
| bootloader | `install_file` | 0x20001630 | 276 | 115/113 | differs | 97% | same | same |
| bootloader | `esp_dma_read` | 0x20016bf8 | 204 | 83/85 | differs | 93% | same | differs (ours 0x40002b) |
| main-ode | `audio_fill` | 0x100816b8 | 1300 | 526/526 | differs | 99% | same | same |
| main-ode | `adpcm_decode_frame` | 0x10081f4c | 812 | 398/394 | differs | 95% | same | differs (ours 0x7fff 0x1fffff) |
| main-ode | `req_submit` | 0x10082370 | 144 | 55/55 | differs | 98% | same | same |
| main-ode | `cmd_read` | 0x20001354 | 876 | 343/343 | differs | 99% | same | same |
| main-ode | `cmd_ipc` | 0x2000178c | 7768 | 3238/2977 | differs | 89% | differs (10) | differs (9 words of ours; orig 0x3fff) |
| main-ode | `qspi_send` | 0x20003a04 | 648 | 267/267 | differs | 99% | same | differs (ours 0x40001b) |
| main-ode | `aud_dma_irq` | 0x20004038 | 504 | 213/213 | register-only | 100% | same | differs (ours 0x2003ccc8) |
| main-ode | `cmd_audio_stream` | 0x20004230 | 1304 | 508/508 | reordered | 100% | same | same |
| main-ode | `di_stream_net` | 0x20004884 | 1988 | 855/819 | differs | 93% | differs (10) | differs (ours 0x2003cd22 0x50200010 0x23ee0 0x2d004; orig 0x100939d4) |
| main-ode | `di_dma_irq` | 0x20005048 | 132 | 13/55 | differs | 20% | differs (3) | differs (ours 0x2003b6b8; orig 0x40c 0x20006758 0x20006754) |

- **Match**: share of instructions found on both sides, with registers, branch targets and
  stack offsets erased and mnemonics and immediates kept.
- **Calls**: the call sequence, as an edit distance.
- **Literals**: literal-pool words. A word only on our side is usually a constant we load
  where the original builds it in registers, which is not a behaviour difference by itself.
- **Original bytes** run to the next known function start, so they can include an unidentified
  neighbour: `di_dma_irq`'s 132 bytes do (ours is 13 instructions), and the `cmd_ipc` and
  `di_stream_net` regions are cut short by a symbol inside them, so their counts are
  approximate. About 700 further copied bytes belong to small unidentified functions between
  known ones (`equiv.py` prints them).

## Targets

| Image | Load | Size | SHA-256 | Status |
|---|---|---|---|---|
| `flippydrive-bootloader.bin` | 0x10000000 | 0x40600 | fe5e5047…a0c64bf7e | exact build identical, 1.4% of bytes copied from the original; C build links (Decompilation status) |
| `flippydrive-main-ode.bin` | 0x10080000 | 0x1b800 | cadba959…ec7be7f34 | exact build identical, 13.4% of bytes copied from the original; C build links (Decompilation status) |
| GameCube disc image (bootloader 0x100002d0) | — | 0x258f4 | 1996f511…7e92af267 | identical (gc-boot-tools `create_iso_header`) |
| GameCube `main.dol` (in the bootloader's disc image) | 0x81100000 | 0x1d8f4 | 1b787f13…b08dc4c49 | identical |
| GameCube `apploader.img` (disc 0x2440) | 0x81200000 | 0x2394 | f9c16679…bad319f6b | identical (gc-boot-tools) |
| GameCube `payload.dol` (gzipped inside `main.dol`) | 0x80003100 | 0x462a0 | b855c27e…1413d76a4 | identical; application 55/55 functions C, 51 matching (the other 4 built from the original's code); constants and strings `.incbin`'d |

## Package contents

`update.fpkg` 1.4.6 is an `FPK1` container of ten entries (0x2d5200 bytes total; format in
`tools/fpkg.py`). Each row below is one entry: its offset in the package, payload length,
header load address, header version, and how it is reproduced. Only the two RP2040 images are
rebuilt by this project; everything else is relayed from the original package by
`tools/fpkg.py --pack`.

| # | Entry | fpkg offset | Length | Load | Ver | Reproduced |
|---|---|---|---|---|---|---|
| 0 | `flippydrive-bootloader.bin` | 0x000200 | 0x40600 | 0x10000000 | 0.4.1 | **this project, byte-identical** (RP2040; embeds the GameCube disc — see below) |
| 1 | `flippydrive-main-ode.bin` | 0x040a00 | 0x1b800 | 0x10080000 | 0.4.1 | **this project, byte-identical** (RP2040) |
| 2 | `esp_bootldr.bin` | 0x05c400 | 0x0ac00 | 0x00000000 | 0.2.0 | original bytes — ESP32 flash-encrypted, no source (`docs/ESP32.md`) |
| 3 | `esp_parttab.bin` | 0x067200 | 0x00e00 | 0x0000f000 | 0.2.1 | original bytes — encrypted ESP32 partition table |
| 4 | `stub.bin` | 0x068200 | 0x008dc | — | 0.0.0 | original bytes — Swiss `STUBHAXX` stub (third-party) |
| 5 | `backup.dol` | 0x068e00 | 0x56340 | — | 0.0.0 | original bytes — FlippyDrive disc-backup tool, no public source |
| 6 | `remote.dol` | 0x0bf400 | 0x42660 | 0x80003100 | 0.0.0 | original bytes — FlippyDrive remote-access tool, no public source |
| 7 | `cubeboot.dol` | 0x101e00 | 0x36cb0 | — | 0.0.0 | original bytes — OffBroadway/cubeboot beta-rc15 (not rebuilt) |
| 8 | `swiss-gc.dol` | 0x138e00 | 0xb36e0 | — | 0.0.0 | original bytes — swiss-gc @40a65cb3 (not rebuilt) |
| 9 | `esp_ode.bin` | 0x1ec800 | 0xe8800 | 0x00060000 | 0.2.2 | original bytes — encrypted ESP32 application, no source |

ESP32 entries (2, 3, 9) carry ESP32 flash offsets as their load address; the DOLs (4–8) hold
their own load addresses inside the DOL format, so the header shows 0. Entries 4–8 are
GameCube payloads; what each is and where its source lives is in `docs/THIRD_PARTY.md`. The
ESP32 entries are flash-encrypted and cannot be rebuilt from source; `docs/ESP32.md` has the
analysis and the only (on-device) route that could decrypt them.

### Inside the bootloader image (entry 0)

The bootloader embeds a bootable GameCube disc, which in turn nests the payload. These are not
separate package entries; they are rebuilt and reassembled to reconstruct
`flippydrive-bootloader.bin` byte-for-byte (SHA-256s and status in the Targets table):

- GameCube disc image, at bootloader offset 0x2d0 (`gamecube/disc`);
- `apploader.img` at disc offset 0x2440, loads at 0x81200000 (`gamecube/apploader`);
- `main.dol` (the GameCube stub) loads at 0x81100000 (`gamecube/stub`);
- `payload.dol`, gzipped inside `main.dol`, loads at 0x80003100 (`gamecube/payload`).

### What the build needs from the original

The 1.4.6 `update.fpkg` must be at `original/update.fpkg` (not in the repository). The GameCube
**payload** build takes a few things from it directly, so that vendor code and art are not
redistributed: the font and logo textures, the code of the four payload functions whose C does
not match yet, and the float constants and strings (all `.incbin`'d). The RP2040 C build takes
nothing from it directly; the RP2040 exact build takes every byte that is not a matching C
function from the original images.

`python -I tools/fpkg.py original/update.fpkg original/extracted` unpacks every entry and
checks its MD5. `python -I tools/gcdisc.py original/extracted/flippydrive-bootloader.bin
original/extracted/gc` extracts the GameCube disc and the payload's textures. `--pack` writes a
new `update.fpkg` (see Build): the eight non-RP2040 entries are copied from the original
unchanged — none is reproduced from source.

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

`wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- bash tools/check_all.sh` runs all of this plus the
exact build and the package, checks every image and `update.fpkg` against the original, and
prints each RP2040 image's bytes by source.

Output in WSL: `~/fd-build/fw/{bootloader,main-ode}/flippydrive-*.bin`,
`~/fd-build/gc-apploader/apploader.img`, `~/fd-build/gc-payload/payload.dol`,
`~/fd-build/gc-stub/main.dol` and `~/fd-build/gc-disc/disc.iso`.

The exact RP2040 build reuses the C build's objects (`docs/EXACT_BUILD.md`):

```
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- python3 tools/rp_exact.py bootloader
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- python3 tools/rp_exact.py main-ode
```

Output in WSL: `~/fd-build/exact-{bootloader,main-ode}/`.

To package the built images, copy them out of WSL and pack them into a copy of the original
(entries are matched by file name; the others are kept):

```
wsl.exe -d Ubuntu --cd F:/flippydrive-fw -- sh -c "mkdir -p build && cp ~/fd-build/exact-bootloader/flippydrive-bootloader.bin ~/fd-build/exact-main-ode/flippydrive-main-ode.bin build/"
python -I tools/fpkg.py --pack original/update.fpkg build/update.fpkg build/flippydrive-bootloader.bin build/flippydrive-main-ode.bin
```

With the exact images the result is identical to the original `update.fpkg`
(SHA-256 867a7fd8…1af0b1b). A package holding C-build images unpacks with valid MD5s, but
whether the updater accepts entries whose size differs from the original's is untested.

See `docs/PLAN.md` for the toolchain evidence and the approach.
