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

## The only route: on-device (needs the physical board)

The key cannot be rebuilt offline. With a FlippyDrive in hand it may be possible to obtain the
decrypted firmware (not the key, which stays read-protected). Espressif's flash-encryption docs
are the reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/flash-encryption.html

1. **Reach the ESP32 ROM download mode.** The bootloader already exposes the ESP32's UART over
   USB (`usb_uart_bridge`) and drives its boot/reset lines (`loader_port_enter_bootloader`,
   `bootloader/esp_port.c`): boot low, reset pulse, boot high. Drive those to drop the ESP32
   into download mode, then talk to it with `esptool.py` / `espefuse.py`.
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

Until then the three ESP32 images stay opaque and are passed through unchanged.
