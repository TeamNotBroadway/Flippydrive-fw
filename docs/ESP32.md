# The ESP32 images

`update.fpkg` carries three ESP32 payloads that this project does **not** reproduce. They are
relayed from the original package as-is (`tools/fpkg.py --pack`). This note records what they
are, why they cannot be rebuilt from source, and the only route that could decrypt them. None
of this is needed to rebuild `update.fpkg`; it matters only to decompile the ESP32 firmware.

The target is an ESP32 (the original, not S2/S3/C3): `esp_loader_connect` forces
`s_target = ESP32_CHIP` (`docs/HACKS.md`).

## The entries

| Entry | fpkg version | ESP32 flash offset | 1.4.6 length | Byte-0 entropy (1.4.6) |
|---|---|---|---|---|
| `esp_bootldr` | 0.2.0 | 0x00000 | 0x0ac00 | 7.98 / 8 |
| `esp_parttab` | 0.2.1 | 0x0f000 | 0x00e00 | 7.34 / 8 |
| `esp_ode` | 0.2.2 | 0x60000 | 0xe8800 | 8.00 / 8 |

The flash offsets are the `img->address` the installer passes to `flash_esp32`
(`bootloader/app_c.c`); it writes the bytes to ESP32 flash unchanged over esp-serial-flasher.

## They are ESP32 hardware flash encryption

Evidence:

- **High entropy, no plaintext.** Each image is near-maximal entropy with no `0xE9` ESP32 image
  magic and no readable partition-table structure. The only repeated 16-byte blocks are
  all-zero — inter-segment padding that was left unencrypted — so this is not AES-ECB (ECB
  turns zero blocks into a fixed non-zero block).
- **Deterministic across versions.** `esp_parttab` is byte-identical in every release from
  1.3.2 to 1.4.6; `esp_ode` is identical from 1.3.2 to 1.4.2 (it changes at 1.4.5). ESP32 flash
  encryption is deterministic — AES-256 tweaked by the flash address, no IV — so identical
  plaintext at the same flash offset yields identical ciphertext. This confirms a single stable
  key shared across all devices and releases.
- **Architectural necessity.** `esp_bootldr` is the ESP32 second-stage bootloader and is itself
  encrypted. The ESP32 mask ROM decrypts flash only in hardware, from the eFuse key; it runs no
  custom software decryptor. So the images can only be ESP32 hardware flash encryption, and the
  key lives in the device eFuse (and OffBroadway's build pipeline), nowhere in the firmware.

## Why the key is not in any public artifact

Checked, all 1.4.6 unless noted:

- `flippy_cli` and `flippy_app` (the shipped host tools): both are thin CLIs over
  `github.com/OffBroadway/go-fatfs` and a device IPC/FTP protocol. Neither contains `FPK1`/
  `FIM1` packing, any flash-encryption or `espsecure` routine, or a key constant. Their only
  AES is Go's TLS stdlib. So neither builds `update.fpkg` nor encrypts the ESP32 images.
- No released `update.fpkg` (1.3.2–1.4.6) ships a plaintext ESP32 image — flash encryption was
  enabled from the first release.
- Because the encryption is real AES, the predictable `esp_parttab` plaintext does not help: a
  known-plaintext pair does not recover an AES key, and there is no keystream reuse.

The packer and the key are in a private build pipeline.

## Determining the chip and silicon revision

We know the **variant** is the original ESP32 (not S2/S3/C3): `esp_loader_connect` forces
`ESP32_CHIP`, which also fixes the flash-encryption algorithm (AES-256 address-tweaked, not
XTS-AES). The variant cannot be narrowed further offline — the RP2040 only records "ESP32", and
the images are encrypted.

The **silicon revision** (v0/v1/v2/v3) can only be read from the board, and it matters: it
decides whether the release-mode hardware key-extraction fallback below is feasible (v0/v1 are
vulnerable to published voltage-glitch attacks, v3 is hardened). Three ways to read it:

- esptool's connection banner prints it, e.g. `Chip is ESP32-D0WD-V3 (revision v3.0)`;
  `tools/esp32_dump.py` runs `esptool flash_id` first and reports the revision and its attack
  implication.
- `espefuse.py summary` shows the `WAFER_VERSION` / `CHIP_VERSION` eFuses it is derived from.
- The chip or module silkscreen: the part number (`ESP32-D0WD-V3` vs `-D0WDQ6`) and date code
  identify the revision without any tooling.

## The only route: on-device (needs the physical board)

The key cannot be rebuilt offline. With a FlippyDrive in hand it may be possible to obtain the
decrypted firmware (not the key, which stays read-protected). Espressif's flash-encryption docs
are the reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/flash-encryption.html

1. **Reach the ESP32 ROM download mode.** The bootloader already exposes the ESP32's UART over
   USB (`usb_uart_bridge`) and drives its boot/reset lines (`loader_port_enter_bootloader`,
   `bootloader/esp_port.c`): boot low, reset pulse, boot high. But `usb_uart_bridge` injects a
   `\r` before every byte equal to `bridge_escape` (BSS-zero, so every `0x00` byte), which
   corrupts esptool/espefuse's binary SLIP framing. Two ways round it:
   - **Patch build** (preferred): `python3 tools/patch_bootloader.py OUTDIR` writes the original
     bootloader with 2 bytes changed: the `beq` that takes the `\r` path (RAM 0x20000198, flash
     0x10025cd0) becomes a `nop`, so the stock `0x12` bridge passes every byte through. Every
     other byte is the original's, so no decompiled C runs; the tool checks that. It also writes
     `OUTDIR/update.fpkg` with that entry replaced. The bridge still sends its `0x06`
     acknowledgement first. Not tested on hardware.
   - **C build:** build with `-DFD_ESP_EXTRACT=ON` and send `0x11` on the USB-CDC control
     channel; `clean_bridge` (`bootloader/app_a.c`) does the same setup and bridges byte for
     byte. This build runs the decompiled C for every function, including the 8 that are not
     byte-identical (README, Decompilation status), so it is less safe. Adding `0x11` to the
     original by patching instead needs a RAM trampoline (`main` runs from RAM and a `bl` cannot
     reach appended flash code), and no free RAM is identified.

   Then run `esptool.py` / `espefuse.py` against that port. Pack either image into a recovery
   image with `tools/recovery.py pack --bootloader` (see [RECOVERY.md](RECOVERY.md)).
2. **Read the eFuse state:** `espefuse.py -p PORT summary`. The decisive bits are the flash
   encryption mode and `DISABLE_DL_DECRYPT` / `DISABLE_DL_CACHE`. `FLASH_CRYPT_CONFIG` is
   readable; the key block is read-protected and will not read back.
3. **If development mode** (`DISABLE_DL_DECRYPT` not set): `esptool.py -p PORT read_flash 0
   <size> flash.bin` returns **plaintext** — the ROM decrypts transparently on download-mode
   reads. This is the whole firmware, decrypted. The ESP32 firmware is nearly frozen across
   versions, so one dump covers essentially all of them.
4. **If release mode** (`DISABLE_DL_DECRYPT` set): download-mode reads return the same
   ciphertext we already have. Options then are a RAM-stub that maps flash through the cache
   (blocked if `DISABLE_DL_CACHE` is set), exploiting the firmware's own IPC protocol for a
   memory dump, or hardware key extraction (voltage-glitch / side-channel; practical on ESP32
   V1/V2, hardened on V3) — all out of scope here.
5. **If the key is ever obtained** (e.g. from a dev-mode device's build or a leaked pipeline
   secret), decrypt offline with `espsecure.py decrypt_flash_data --keyfile key.bin --address
   <offset> --flash_crypt_conf <cfg> <image> -o plain.bin`, using the flash offsets above.

`tools/esp32_dump.py --port <PORT>` automates steps 2–4: it reads the eFuse summary, decides
from the flash-encryption mode whether download-mode reads will be plaintext, dumps the flash,
and classifies the three regions (0xE9 = decrypted). It is read-only (no eFuse burn, no flash
write) and needs `pip install esptool`. It does not itself toggle IO0/EN, so the board must
already be in download mode: either direct wiring, or the clean-bridge command above (which does
drive the lines) on an `-DFD_ESP_EXTRACT` build. Its header documents both. Untested — no board
was available when it was written.

## Writing to an encryption-enabled ESP32, and the reflash budget

This is the inverse of reading, and it constrains step 1 above: flashing the
`-DFD_ESP_EXTRACT` bootloader onto a device that already has flash encryption enabled is **not**
a plain write.

- **A plaintext write does not boot.** The flash controller decrypts every fetch transparently
  from the eFuse key, so plaintext bytes written to an encrypted partition are "decrypted" into
  garbage on read. The image has to land in flash as ciphertext under *this device's* key — the
  key we do not have offline. So the stock `flash_esp32` path (which writes the fpkg bytes
  unchanged) only produces a bootable result for images that were already encrypted by
  OffBroadway's pipeline.
- **Development mode** is the only UART route for a self-built image. `esptool.py write_flash
  --encrypt` has the ROM encrypt each block device-side during download, reusing the eFuse key,
  so plaintext you supply lands as valid ciphertext. This is how a modified bootloader could be
  flashed without the pipeline key — but only on a dev-mode device.
- **Release mode** (`DISABLE_DL_DECRYPT` set) blocks this too: encrypted-download is disabled, so
  there is no UART path to write a working image at all. A modified bootloader would then have to
  go through the firmware's own update path, not esptool.

**The reflash budget.** `FLASH_CRYPT_CNT` is a 7-bit eFuse on the original ESP32 (newer variants
use a 3-bit `SPI_BOOT_CRYPT_CNT`). Encryption is enabled when an odd number of its bits are set;
each re-encryption toggles parity by burning one more bit. Bits only go 0->1, so the number of
re-encryption cycles is small and finite — a handful (on the order of 3) on the original ESP32
before the counter is spent and the partition is locked as-is. Treat the exact remaining count as
something to read from the board, not to assume.

**What spends a cycle.** Not the UART transfer — that is only transport, and reflashing identical
ciphertext never moves the counter. A bit is burned by the *encryption operation*: the bootloader
taking plaintext in flash and encrypting it in place with the saved key, then advancing
`FLASH_CRYPT_CNT` to record it. So a plaintext reflash (write plaintext -> reboot -> bootloader
re-encrypts -> burn) costs a cycle, whereas an already-encrypted `--encrypt` download generally
does not. The key in eFuse block 1 is burned once and is separate from the counter; it is reused
every cycle, never re-burned. Confirm empirically with `espefuse.py summary` before and after a
write: if the set-bit count of `FLASH_CRYPT_CNT` rose, that operation cost a cycle. This matters
for extraction because the budget is shared and irreversible — spending it on bootloader-swap
experiments is a one-way door.

Until then the three ESP32 images stay opaque and are passed through unchanged.
