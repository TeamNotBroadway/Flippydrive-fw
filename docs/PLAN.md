# Plan

## Package contents (update.fpkg 0.4.1)

| Entry | Type | Arch | In scope |
|---|---|---|---|
| flippydrive-bootloader 0.4.1 | 0x000 | RP2040, flash 0x10000000, boot2 included | yes |
| flippydrive-main-ode 0.4.1 | 0x101 | RP2040, flash 0x10080000, no boot2, own vector table | yes |
| esp_bootldr 0.2.0, esp_parttab 0.2.1, esp_ode 0.2.2 | 6/7/9 | ESP32 | no: encrypted (no 0xE9 magic, zlib ratio 0.95-1.00) |
| GameCube disc in the bootloader | — | PowerPC, see below | yes |
| stub.bin | 0xff | PowerPC (GameCube), "STUBHAXX" | later, optional |
| swiss-gc, cubeboot, remote, backup .dol | 0xff | GameCube DOLs | no: third-party payloads |

## GameCube disc inside the bootloader

The bootloader's flash region 0x100002d0-0x10025bd0 is a GameCube disc image the drive
serves: id `GBLAOB`, name `GAMECUBE COMPAT BOOTLOADER`.

- apploader at disc 0x2440: date `2023/07/23`, entry 0x81200000, 0x2374 bytes; plain code
  (strings: DOL header checks, "Found dol", "fst loaded").
- FST: one file, `opening.bnr` (disc 0x6480, 0x1960 bytes).
- DOL at disc 0x8000 (`main.dol`): one text section, 0x1d7f4 bytes at 0x81100000, bss
  0x8111d7f4+0x440c. It is a loader stub: devkitPPC-style crt0 (`_arg` at +4), about 5.5 KB
  of code, then a gzip stream at text+0x1600 (0x1c0d5 bytes), then a CRC-32 table. The stub
  gunzips into 0x81121c00, checks the length against the gzip trailer, copies the sections
  of the resulting DOL into place, clears its bss and jumps to its entry.
- The stub is **built from source, byte-identical**: `gamecube/stub` is OffBroadway/cubeboot's
  `entry/` at 873893b (branch beta-rc15) with two changes found by matching: link base
  0x81100000 (`nosys.ld`; cubeboot uses 0x81300000) and tinf's data-error returns numbered
  (`TINF_DATA_ERROR - 10*k`: k = 1-13 in `tinf_gzip_uncompress`, 20-21 in
  `tinf_uncompress`; other returns unchanged). tinf is v1.2.1. Built with devkitPPC r47 at
  `-Os` (identical output with r47 and r47.1); newlib `memset` and libgcc `_restgpr_*_x`
  come from the toolchain.
- The gzip stream is GNU gzip at the default level (-6) of a file named `updater.dol` with
  mtime 0x6842765e (2025-06-06 05:02:22 UTC): `gzip -6` (gzip 1.12, Ubuntu) of the
  decompressed payload with that name and mtime reproduces all 0x1c0d5 bytes.
- `payload.dol` (decompressed, 0x462a0 bytes): text 0x80003100+0x272e0, data
  0x8002a3e0+0x1eea0, bss 0x80049280+0x82e0, entry 0x80003100. The GameCube-side boot menu:
  strings "Boot Onboard DOL", "Update/Reinstall Firmware", "/cubeboot.dol", "/stub.bin",
  network boot; newlib locale strings. Its exception handler strings
  (`


	Exception (%s) occurred!
`, `
	Reloading in %d seconds
`) are libogc2's
  (extremscorner) before commit 8432a8f (2025-08-27), not devkitPro libogc's. libogc2 added
  devkitPPC r47 support at fd5410c (2025-05-25).
- Payload toolchain: **devkitPPC release 47.1 (GCC 15.1.0)** with **libogc2 4887481-70f80d8**
  (these commits build identical GameCube libraries). The payload has `__flush_cache` in
  `system_asm.S`, which 3588a9b (2025-06-03) removed. r47.1 is the package in
  Docker image `devkitpro/devkitppc:20250727`; its newlib has positional printf arguments
  (buildscripts "devkitPPC: enable positional arguments", 2025-06-04), and the payload's
  `get_arg` (1980 bytes) and `_svfprintf_r` (14840 bytes) match it exactly but not r47's.
  libogc2 builds identically with r47 and r47.1. Sweep of libogc2 commits: 295 functions at
  fd5410c-4d32cb0, 294 at 546f150, falling to 250 by 6e635fe.
- `tools/match_gc.sh` names 517 payload addresses: libogc2 including its assembly
  (symmatch sizes unsized assembly labels by the next symbol), newlib, libsysbase, libgcc and
  devkitPPC's `crtmain.o`.
- **The payload links byte-identical** (`gamecube/payload`): an assembly stand-in for the
  application (`tools/gc_appasm.py`) linked with `-logc -lm` against libogc2 70f80d8 by
  devkitPPC r47.1's `ogc.ld`. So every library function and all library data are unmodified
  libogc2, newlib, libsysbase and libgcc; nothing needs vendoring.
- The application, from the original's `.eh_frame`: 55 functions at 0x8000533c-0x80009484
  (between `__crtmain` and libogc2's `__lwp_isr_in_progress`), compiled with
  `-ffunction-sections -fdata-sections` and unwind tables. Its data: two TPL textures
  (0x8002a440 960x16 RGBA8, 0x80039480 380x32 RGB5A3, 32-aligned, no size words), 4-byte
  float constants and strings (merged with the libraries'), one 16-entry jump table, a word
  in `.sbss2` (0xcc008000), three words of `.sdata` (0x14, 0xa, 0xcc006000), 0x1c bytes of
  `.sbss` and 0x1d00 of `.bss`. It reaches its small data with `lis`/`addi`, not through
  r13/r2. The menu's strings are not on GitHub, but part of its code is OffBroadway/cubeboot
  (below).
- OffBroadway sources (clones in `C:\Tools\offbroadway`), checked against the images:
  - cubeboot beta-rc15, compiled with devkitPPC r47.1 at -O2 (`-ffunction-sections
    -fdata-sections`, libogc2 70f80d8 headers): 20 payload addresses match exactly, from
    `cubeboot/source/flippy_sync.c` (`dvd_inquiry`, `dvd_bootloader_status`/`boot`/
    `update`/`noupdate`, `dvd_custom_close`, `dvd_read`, `dvd_custom_unlink[_flash]`,
    `dvd_custom_open[_flash]`, `dvd_custom_bypass`, 0x800059dc-0x800061e4),
    `patches/source/gc_dvd.c` (`dvd_cover_status`, 0x80006350), `cubeboot/source/boot/
    sidestep.c` (`ARAMStub`, `ARAMRunStub` and helpers, 0x80008a68-0x80008c1c) and
    `boot/ssaram.c` (`__ARWriteDMA`, `ARAMFetch`, 0x80009074-0x80009204).
    custom-loader-menu gives the same.
  - gc-boot-tools (GameCube disc tools): `icons/opening.bnr` equals the disc's on branches
    ntsc11-debug-stock through save-ipl-fastboot. `create_iso_header` writes "GAMECUBE
    COMPAT BOOTLOADER" and the apploader date 2023/07/23 on branches stock-compat-apploader
    and later. Its `ppc/apploader` at 8d59fb2 (save-ipl-fastboot), built with its Makefile
    (`-O2`) by devkitPPC r47.1, matches 30 of 31 functions (4948 bytes) exactly; `memset`
    (`ppc/common/lib.c`, 112 bytes) differs, and the image is 0x23a0 bytes against the
    original's 0x2374.
  - swiss-gc flippy-edits: `cube/swiss/source/devices/flippydrive/flippy.[ch]` speak the
    same DI protocol as the payload (command 0xB5, magic values 0xABADBEEF, 0xCAFE6969,
    0xDABFED69, 0xDECAF420) through libogc's DVD layer, which the payload does not link: a
    reference for names, not a byte source. Its version line uses "-dirtyboi" like the menu.
  - pico-sdk fdfixes (tag 2.1.1-fdfix1) is 2.1.1 plus a host-side pioasm build fix for GCC
    15: the firmware's SDK code is plain 2.1.1.
  - go-fatfs carries stock FatFs R0.15a w/patch1. Built in place of R0.15 with the same
    ffconf, its window score against the firmware is lower (bootloader 1730/4902 against
    1821/4824; main-ode 1355/4041 against 1381/3896), so the firmware's FatFs is not it.
  - flippydrive (docs: `source/di.rst`, `fdi.rst`, `bootloader.rst`, `updates.rst`) documents
    the DI protocol and update flow. flippydrive-assets holds only a README.
  - Not firmware: gekko-logger (Go host tool reading a USB Gecko), exfat (FUSE exFAT),
    gozstd, kcp-go, libui-ng, NKit (host libraries and tools), cleanrip, omega-kernel
    (GBA), parse-gcm, cubeboot-mockup (HTML), copy_file_to_another_repo_action.
- The stub links libgcc's `_restgpr_*_x` and newlib's `memset` exactly (devkitPPC GCC, no
  libogc). The apploader builds byte-identical from `gamecube/apploader`: gc-boot-tools
  8d59fb2's `ppc/apploader` and `ppc/common` with its Makefile's flags and devkitPPC r47.1,
  except `ppc/common/lib.c` without `-fno-builtin` (GCC then turns `memset`'s loop into a
  call to `memset`, as in the original).
- `tools/gcdisc.py` extracts all parts to `original/extracted/gc/`.
- No toolchain strings in the apploader or the stub; none of their strings are on GitHub except one DOL check
  message also used in leonardus/ORCA freeloader.

## Build environment, from the binaries

Both RP2040 images carry pico-sdk `binary_info`:

- pico-sdk **2.1.1**, `PICO_BOARD=pico`, `boot2_w25q080`, CMake build type **Release**
- stdio over USB (TinyUSB, as bundled with SDK 2.1.1)
- built `Jun  6 2025`
- libc: newlib 4.5.0.20241231 from **Arch Linux `arm-none-eabi-newlib` 4.5.0.20241231-1**.
  Evidence: the assert path `../../../../../../newlib-4.5.0.20241231/newlib/libc/stdlib/dtoa.c`
  matches Arch's libc.a exactly; Debian (`../../../../../../newlib/...`), MSYS2
  (`C:/M/B/src/...`) and Arm 14.2.Rel1 (`/arm-14-5/src/newlib-cygwin/...`) differ.
  Function sections of v6-m/nofp libc.a found verbatim in the two images (relocations masked):
  Arch 94 functions / 24520 bytes, Arm 14.2.Rel1 93 / 15512, MSYS2 44 / 3044, Debian 40 / 2780;
  every libc_nano.a 38 or fewer. So full libc, not nano.
- Compiler: **Arch `arm-none-eabi-gcc` 14.2.0-1** (binutils 2.43-1 assumed: what Arch shipped
  on Jun 6 2025). Evidence: a pico-sdk 2.1.1 Release project (USB stdio, pico_stdlib, flash,
  dma, pio, spi, i2c, uart, multicore) built with identical flags by both compilers; 625
  function sections each. Found verbatim in the images: Arch 228 (36486 bytes), Arm 14.2.Rel1
  224 (35626). Arch-only hits: `sleep_ms`, `sleep_us`, `usbd_open_edpt_pair`,
  `tu_edpt_stream_read_xfer`; no Arm-only hits. The other ~400 are not in the images or
  differ through config/inlining; not yet examined.
- Startup check (`ref/` built with `tools/wsl_build.sh`, compared with `ref.bin`):
  boot2 0x10000000-0x10000100 and the bootloader's vector table are byte-identical; crt0
  instructions are identical except address and literal-offset fields. The bootloader's
  `ldr` offsets are all +3 words: its `data_cpy_table` has one more region than a stock
  link (0x1003bd20 -> 0x200161f0..0x2001a878), so the linker script adds a section.
  main-ode's crt0 differs from stock only by its 0x10080000 base. In both originals the
  binary_info strings follow crt0 directly, where a stock link puts `main`.
- Layout (reproduced by the top-level project, see below):
  - bootloader: `PICO_COPY_TO_RAM` (code runs from RAM; copy table text/data/scratch_x/y).
    `memmap_bootloader.ld` = SDK `memmap_copy_to_ram.ld` + `.image_header` + `.flash_blob`.
  - main-ode: SDK `memmap_default.ld` at 0x10080000 without boot2 + `.image_header`.
  - `common/image_header.h`: 52-byte header after crt0 (name, version, markers).
  - Program name and version come from `pico_set_program_name`/`_version`; without the name
    binary_info has 8 entries instead of the original's 9. `__DATE__` from
    `SOURCE_DATE_EPOCH` (set in `tools/wsl_build.sh`).
  - Check: boot2, vectors, crt0, copy table and image header are identical to the originals
    except 11 address words in each image (values that move with the program's size);
    binary_info range ends at the original's 0x10025bf8 in the bootloader.
- bootloader runs `.data` from RAM (copy table at 0x10000238: 0x16128 bytes to 0x200000c0,
  0x4688 bytes to 0x200161f0).

## Installed tools

| Tool | Path |
|---|---|
| Arch gcc 14.2.0-1, binutils 2.43-1, newlib 4.5.0.20241231-1 (Linux; run under WSL Ubuntu; v7/v8 multilibs and docs left out) | `C:\Tools\arch-arm-none-eabi-14.2.0` |
| CMake 3.31.8 and Ninja 1.12.1, Linux builds for WSL (CMake sha256 checked) | `C:\Tools\cmake-3.31.8-linux-x86_64`, `C:\Tools\ninja-linux-1.12.1` |
| Arm GNU Toolchain 14.2.Rel1 (sha256 matches Arm's; Windows; used to generate CMake builds) | `C:\Tools\arm-gnu-toolchain-14.2.rel1` |
| pico-sdk 2.1.1, TinyUSB 86ad6e5 | `C:\Tools\pico-sdk-2.1.1` |
| Ghidra 12.2 (needs Java 25+: `C:\Tools\openjdk-26.0.2_windows-x64_bin\jdk-26.0.2`) | `C:\Tools\ghidra_12.2_PUBLIC` |
| devkitPPC r47.1 (GCC 15.1.0), libogc 2.12.2 and devkitPro tools: `/opt/devkitpro` of Docker image `devkitpro/devkitppc:20250727` (layer sha256 checked against the manifest; Linux, run under WSL). Used for all GameCube builds | `C:\Tools\devkitppc-20250727\root\opt\devkitpro` |
| devkitPPC r47 (GCC 15.1.0), image `devkitpro/devkitppc:20250527`: superseded by r47.1 (its newlib lacks positional printf arguments) | `C:\Tools\devkitppc-20250527\root\opt\devkitpro` |
| libogc2 (extremscorner), git clone (`tools/match_gc.sh` builds 70f80d8 from it) | `C:\Tools\libogc2` |
| OffBroadway repositories pushed since 2024, plus wii-apploader: full git clones | `C:\Tools\offbroadway\<repo>` |

## Tools in this repo

- `tools/fpkg.py`: unpack update.fpkg (the bootloader build `.incbin`s from the result).
- `tools/compare.py BUILT ORIG BASE`: equal byte count and differing ranges.
- `tools/symmatch.py`, `tools/match_libs.sh`: name library functions in the originals
  (ARM or PowerPC objects; RP2040 images or DOLs).
- `tools/nearmatch.py`: locate library functions that are close but not identical.
- `tools/funcdiff.py`, `tools/confsweep.py`: per-function closeness; library config search.
- `tools/gcdisc.py`: extract the GameCube disc, unpack the payload DOL, write ELF wrappers
  (`tools/dol2elf.py`).
- `tools/match_gc.sh`: build libogc2 with devkitPPC r47.1 and name payload library functions.
- `tools/gc_appasm.py`: the payload's application data not yet in C, and the original's code for the functions whose C does not match.
- `tools/dolcmp.py`: compare two DOLs (sections, differing ranges).
- `tools/ghidra_import_gc.sh`, `tools/ghidra/GcSetup.java`: Ghidra import of the GameCube
  ELFs (PowerPC:BE:32:default; Ghidra has no Gekko variant) and C export.
- `tools/ghidra/FdSetup.java`, `FdExport.java`, `tools/ghidra_import.sh`: Ghidra import
  (memory map from the copy table, RP2040 peripheral blocks, Thumb), symbols, C export.
- `tools/libmatch.py LIB.a FW...`: count archive functions found verbatim in the images.
- `tools/objmatch.py DIR OUT.tsv FW...`: same for every `.obj` under a build directory.
- `tools/wsl_build.sh SRC NAME`: configure and build a pico-sdk project in WSL with the Arch
  toolchain, into `~/fd-build/NAME`.
- `tools/gen_wsl_compile.py`: replay a Windows CMake build's compile commands with Arch gcc
  (superseded by `wsl_build.sh`; kept for builds that must configure on Windows).

## Goal for the code

As much as possible is C. Library code comes from the library's real source, at the version
and configuration found by matching. Where the original's library code differs from every
public version (FatFs, esp-serial-flasher, and any libogc2 function that does not match),
the library is vendored into `third_party/` and the differing functions are decompiled into
that copy, edited until they compile to the original's bytes. Application functions are
written as C and checked against the original one by one; assembly is only a temporary
stand-in for a function not yet matched, never the final form.

## Analysis status

`tools/match_libs.sh` (WSL, after building `ref/`) names library functions in both images by
masked byte search; `tools/ghidra_import.sh` (Git Bash) imports both into `ghidra/`, applies
those names plus the vector table and crt0 entries, and exports Ghidra's C for every
remaining function to `decomp/<image>/app.c` with a function list in `functions.tsv`.

| Image | Library functions (Ghidra size) | Remaining functions (Ghidra size) |
|---|---|---|
| bootloader | 314, 43272 bytes | 648, 85058 bytes (321 run from RAM) |
| main-ode | 297, 39128 bytes | 349, 41978 bytes |

Library functions include exact matches (newlib, libgcc, SDK, TinyUSB, the vendored
libraries) and near matches for FatFs and esp-serial-flasher, located by
`tools/nearmatch.py` (Ghidra comment "near match"). "Ambiguous" exact matches (identical
code under several names, e.g. newlib's `_read_r`/`_write_r` family) list every name.
Neither FatFs nor esp-serial-flasher is byte-identical to any public source:

- FatFs (both images; exFAT and mkfs on, strings `NO NAME    FAT32`). Only `ff_wtoupper`,
  `ff_uni2oem`, `ff_oem2uni` (ffunicode.c, CP437) match exactly in any of R0.14-R0.15b, so
  the compiler flags are right (`ff_wtoupper` matches at -O3/-O2 only) but ff.c's config or
  source differs. `tools/confsweep.py --fuzzy` (greedy over 24 ffconf options, scored by
  8-byte windows) ranks R0.15 highest for the bootloader (2327 vs 1806-2231). Best config:
  `third_party/fatfs/ffconf.h`. Building without `-fdata-sections` is required (section anchors);
  remaining differences are register allocation and scheduling. Fedora's GCC 14.1.0
  gives the same output as Arch 14.2.0, and GCC 15.1.0 matches worse (1273 windows vs
  2237), so the compiler is not the cause; ff.c is probably modified.
- esp-serial-flasher (bootloader, RAM 0x200099fa-0x2000a8ec). No tag v1.0.0-v1.8.0 matches
  beyond 4-5 small functions. The string `ESP8266 does not support change baudrate
  command.` exists only between commits 5264d5a and adc18ff (first in v1.0.0), in
  `examples/common/example_common.c`. Scoring every first-parent commit in that range
  (MD5 off scores one function more than on): `esp_targets.c` matches best from 40ce22e
  to 3937d49 (before ESP32-C2 support), 21 functions exact; `serial_comm.c` matches better
  after 99fd1eb (RAM loading), while `esp_loader.c` matches poorly everywhere (52 windows).
  So the original is a locally modified fork; the firmware also has strings in neither
  (`current_baud = %d`, `Invalid flash image segment!`).

Both are vendored in `third_party/` from the closest public source (FatFs R0.15 with the best
config, esp-serial-flasher 3937d49 with MD5 off) and will be decompiled like application code:
each function edited until it compiles to the original's bytes, starting from the near-match
locations in `syms/*.lib.tsv`. Until then they are unmodified (`docs/HACKS.md`).

## Approach

Done:

- RP2040: toolchain (Arch GCC 14.2.0, pico-sdk 2.1.1) found; skeleton builds link at the
  original addresses (`tools/wsl_build.sh . fw`); library functions named in Ghidra.
- FatFs R0.15 and esp-serial-flasher 3937d49 vendored as the closest public sources.
- GameCube disc extracted; stub, gzip stream and payload understood; payload toolchain
  (devkitPPC r47.1, libogc2 70f80d8) found; all three images load in Ghidra.
- GameCube stub `main.dol` builds byte-identical from `gamecube/stub` (cubeboot `entry/`).
- GameCube apploader builds byte-identical from `gamecube/apploader` (gc-boot-tools).
- GameCube `payload.dol` links byte-identical from `gamecube/payload`; the stub built around
  it is byte-identical. All 55 application functions are C: cubeboot's
  (`gamecube/payload/source/cubeboot`, edits listed in its README), `gfx.c`, `flippy_ext.c`,
  `video.c`, `main.c` and `textf.c`. 51 match (13768 of 16712 bytes); `boot_menu`,
  `boot_main_app`, `main_menu` and `main` build from the original's code (`docs/HACKS.md`).
  All variables are C; the textures link from `source/textures.s` (extracted by
  `tools/gcdisc.py`); the float constants and strings are still `.incbin`'d.
- GameCube disc builds byte-identical from `gamecube/disc` (gc-boot-tools
  `create_iso_header` and `opening.bnr`, with the apploader and stub built here); the
  bootloader includes it (`bootloader/flash_blob.S`). One 0x220-byte padding range is copied
  from the original (`docs/HACKS.md`).

Next, in order:

1. GameCube payload: match the four `NONMATCHING` functions in `main.c` (removing them
   from `ASM_BODIES` in `tools/gc_appasm.py`), which brings the strings into C; then the
   float constant order (`docs/HACKS.md`). Keep `payload.dol` identical.
2. RP2040 bootloader and main-ode application code: decompile function by function.
   main-ode's own code is 0x100801e8-0x100829e0; the other unnamed functions between
   library runs are library statics. Done (exact unless noted): `diskio.c` (FatFs glue and
   mount), `ioexp.c` (I2C port expander at 0x20), `di.c` (`bus_release`, `di_receive`,
   `di_error`, `di_init_handlers`), `log.c` (log buffer pool; the `LOG` macro in `log.h`
   reproduces the inlined logging), `main.c` (`main`, near match; `set_cmd_handler`).
   `audio.c`/`adpcm.c` (DVD audio streaming and DTK ADPCM; two near matches).
   `ext.c` (ESP link requests, inih-style INI parser; 0x10082370-0x100829e0, 19 of 20).
   RAM code: `ram_a.c` (DI handlers, `di_worker`, `disk_write`), `ram_b.c` (ESP QSPI link),
   `ram_c.c` (`di_gpio_irq`, `bypass_enter`), `ram_d.c` (`cmd_seek`, `cmd_read`, `cmd_wkf`),
   `ram_e.c` (`cmd_ipc`, the 0xB5 handler, 0x2000178c-0x200035cc), `ram_f.c` (audio DMA IRQ,
   `cmd_audio_config`, `di_setup`, `di_dispatch`). Flash: `bus_acquire`, `audio_reset`.
   61 of 81 scored functions exact. Not yet written: `di_stream_net` 0x20004884,
   `config_ini_handler` 0x10080ae8, the function that registers the DI handlers.
   Bootloader (all code runs from RAM): `app_a.c`, `app_b.c` (`main` 0x20000af0, update and
   boot flow), `app_c.c` (filesystem, update.fpkg install, ESP32/RP2040 flashing, handler
   registration), `app_d.c` (DI handlers and core 1, the `.time_critical` run at 0x200161f0).
   Not located: the esp-serial-flasher port layer. The images do not yet place C at the original addresses.
3. FatFs and esp-serial-flasher: decompile the near-matched functions into the vendored
   copies until they match.
4. Test harness for the GameCube `.dol`s (stub, payload, `NONMATCHING` builds): boot them
   in a GameCube emulator with a scriptable debugger (Dolphin's GDB stub, or a Dolphin
   build with scripting), set breakpoints and compare state against the original build.
   Needed before `NONMATCHING` C is trusted: matching bytes prove nothing about a
   non-matching build. Dolphin will likely need changes to emulate the FlippyDrive's side
   of the drive interface; an RP2040 emulator running the main-ode image could later stand
   in for it. Candidate: [Bramble](https://github.com/Night-Traders-Dev/Bramble) (C, MIT;
   dual core, PIO, DMA, USB CDC, XIP; GDB stub; standalone program, not a library; PIO and
   DMA timing are functional models, per its README).

Throughout: unmatched functions may sit as `.s` extracted from the original so each image
builds identical; progress is the share of bytes coming from C. `tools/compare.py` (and
`tools/funcdiff.py` per function) list the first mismatch.
