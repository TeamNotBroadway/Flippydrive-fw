# Hacks

Placeholders and guesses that a byte-identical build currently depends on.

- `bootloader/flash_blob.S`: the GameCube disc (0x100002d0, 16-aligned) and its length
  (0x258f4, at 0x10025bd0) lie inside the binary_info range
  [__binary_info_start, __binary_info_end), ahead of the SDK entries; how the original got
  them there is unknown. `memmap_bootloader.ld` places a `.flash_blob` section there.
- `gamecube/disc`: disc 0x7de0-0x8000, the padding before the DOL, is a constant in the
  vendored `create_iso_header.c`, copied from the original disc. Upstream leaves it
  uninitialised, and the original's bytes there are x86-64 stack contents (pointers of the
  form 0x7ffe...).
- `main-ode/pending.c`: no function placeholders remain. It defines globals whose owning code
  is not identified (`root_dir`, `di_file`, `disc_current`, `ipc_cfg`, `id_bitmap` with a
  guessed size of 32, and others) and `ai_program`, which holds the
  original's audio PIO program as raw words (struct at 0x10093d10).
- `disc_is_file` returns `v != 64` (exact); `ram_d.c`/`ram_e.c` comments that call one
  branch the network path are unverified.
- `ram_c.c` (`bypass_program`) and `ram_f.c` (`di_program`, `di_dir_program`) hold PIO
  programs as raw instruction words from the image, not `.pio` source.
- `bootloader/esp_port.c` declares `expander_set(uint8_t, bool)`; `app_a.c` defines it with
  `uint pin`. The calls match only with the narrow prototype. `esp_flash_finish` sends SYNC,
  not FLASH_END, as the original does. The bootloader built from this tree is not tested.
- `bootloader/app_c.c` holds `di_program` as raw words from the image (0x20018eb8).
- Near matches written by the batch pass: `ext.c` `req_submit` (7/30), `ram_a.c`
  `di_worker` (148/179, two spill slots swapped) and `di_stream_file` (85/118), `ram_b.c`
  `flush_task` (16/24, `ram_g.c`), `qspi_send` (39/213);
  `ram_c.c` `bypass_enter` (130/132); `ram_d.c` `cmd_read` (146/230, block layout);
  `ram_e.c` `cmd_ipc` (1294/2483, case-body order differs); `ram_f.c` `cmd_audio_config`
  (287/379), `aud_dma_irq` (82/192), `id_bitmap_test` (8/19),
  `id_bitmap_set` (1/13).
  Global names in `ram_a.c`/`ram_b.c` that the worker guessed by role (QSPI SM/offset
  assignment, `esp_dma_ch`, `esp_remaining`) are unchecked.
- `main-ode/qspi.c`: `qspi_init` does not match yet (105 of 232 windows): register
  allocation and block order differ from the first log call on.
- `main-ode/audio.c` `audio_fill` (163 of 377 windows) and `main-ode/adpcm.c`
  `adpcm_decode_frame` (same size, 10 of 396: GCC unswitches its loop on a different
  filter test) do not match yet.
- Both linker scripts add an `.image_header` section after `.reset`. The original may instead
  have used an existing section name; the bytes are the same either way.
- `third_party/fatfs` (R0.15) and `third_party/esp-serial-flasher` (3937d49) are the
  public sources. Measured with `tools/libmatch.py` (function sections found byte-for-byte in
  the bootloader image):
  - esp-serial-flasher: the fork's changes were confined to `src/esp_loader.c`, now matched
    (Ghidra-decompiled from the original, `tools/ghidra/FdDecompAt.java`):
    - `esp_loader_connect` (0x20009990): forces `s_target = ESP32_CHIP` after
      `loader_detect_chip` (dropping the ESP8266 branch) and retries sync every 10 ms, not 100.
    - `esp_loader_flash_start` (0x20009a04): adds `printf("Detected flash size: %u\n", ...)`
      after `detect_flash_size` succeeds, before the image-size check. The printf's placement in
      `esp_loader_flash_start` (not in `detect_flash_size`) is what keeps `flash_size` live in
      the frame and makes the stack layout match.
    Both are byte-exact (13/13 and 148/148 non-relocated windows). The MD5 code,
    `loader_md5_cmd` and `loader_flash_end_cmd` are absent from the image (`MD5_ENABLED=0`,
    dead-code-eliminated), not modified. The format strings are not byte-verified (they are
    masked relocations into `.rodata`; the exact build takes the strings from the original).
  - FatFs: libmatch finds 1 of 30 verbatim, but that is the section-anchor data addressing
    (noted in `third_party/fatfs/CMakeLists.txt`), not a modification; FatFs is effectively
    unmodified.
- `gamecube/payload/source/main.c`: `boot_menu`, `boot_main_app`, `main_menu` and `main`
  have C under `NONMATCHING` that does not compile to the original (`main_menu`: the spill
  slots of the logo address and the version format string are swapped. Their costs tie, so
  LRA orders them by pseudo number, and ours creates the format string's first, at expand,
  and the logo's later, in loop2_invariant. The others differ in block layout and register
  allocation). The default
  build replaces each body with the original's code, word for word, from
  `tools/gc_appasm.py` (`ASM_BODY`); only branches to other functions are symbolic, so
  what those bodies address by number is marked `ASM_REF` and referenced from `main.inc`
  with `R_PPC_NONE`. `make NONMATCHING=1` builds all four from C; it links, but is not
  identical and has not been run.
- `gamecube/payload`: the application's float constants (0x8003f3c0-0x8003f400) and strings
  (0x8003f478-0x8003fd58, with the libraries' after them) are `.incbin`'d from
  `original/extracted/gc/payload.dol` into `app0.s` (`tools/gc_appasm.py`), the first object
  with merged sections, so those start with the original's. The strings come out of C in the
  original order once the four `NONMATCHING` functions are C (checked with
  `make NONMATCHING=1`: all data identical but those functions' `.eh_frame` entries). The
  constants do not: gfx.c's pool has 20.0, 0.85, 1.0 where the original has 1.0, 0.85, 20.0,
  followed by a 0.5 that no application code loads (unexplained; possibly from a function
  `--gc-sections` dropped). Every draw_text variant tried that reorders the pool changes its
  code.
