# recovery.uf2

`recovery.uf2` is the BOOTSEL full-flash image: drag it onto the RP2040's mass-storage drive and
it rewrites the whole 16 MB flash. Unlike `update.fpkg` (which the running firmware applies), it
is a flat UF2 of the flash itself, so it includes the bootloader, main-ode, the update package and
the FAT storage region in one file. This project reproduces it byte-for-byte from our builds.

## Layout

An RP2040 UF2 (family `0xe48bff56`), 13408 blocks / 6864896 bytes, in three flash segments:

| Flash address | Contents |
|---|---|
| `0x10000000` | **bootloader** — byte-identical to our exact build (`tools/rp_exact.py bootloader`) |
| `0x10075000` | a 4-entry **FPK1 manifest** (bootloader / main-ode / esp_bootldr / esp_parttab, headers only — "data not present"), then **main-ode** at `0x10080000` (byte-identical to our exact build) |
| `0x10300000` | a **FAT filesystem** (`mkfs.fat`) holding the full 10-entry `update.fpkg` at offset `+0x9000` (flash `0x10309000`) and the boot DOLs |

Each firmware region matches our build exactly up to its trailing zero padding: `recovery.uf2`
leaves the final bytes of each image `0xFF` (erased flash) where our `.bin` / the `update.fpkg`
carry `0x00`. The embedded `update.fpkg` is itself what `tools/fpkg.py --pack` rebuilds
byte-identical from our two images (see [EXACT_BUILD.md](EXACT_BUILD.md) and the fpkg inventory in
the README).

The FAT filesystem — its volume id, timestamps and the vendored DOLs — is not reproducible from
source and is not rebuilt. It is relayed from the stock image, exactly as `fpkg.py --pack` relays
the third-party entries it cannot regenerate.

## Reproducing it

`tools/recovery.py` operates on the UF2 at block granularity: it relays every 512-byte block
verbatim and overwrites only the payload bytes that fall inside a substituted region, so the
generator's exact framing — block order, numbering, and the mixed `0xFF`/`0x00` structural padding
it left after each payload — is preserved. Trailing `0x00` on each substituted image is trimmed so
it maps onto the erased `0xFF` tail.

Verify that our builds reproduce the firmware in a stock image:

```
python3 tools/recovery.py verify recovery.uf2 \
    --bootloader ~/fd-build/exact-bootloader/flippydrive-bootloader.bin \
    --main-ode   ~/fd-build/exact-main-ode/flippydrive-main-ode.bin \
    --fpkg       original/update.fpkg
```

Regenerate it (byte-identical to stock when the inputs are our matching builds):

```
python3 tools/recovery.py pack recovery.uf2 out.uf2 \
    --bootloader ~/fd-build/exact-bootloader/flippydrive-bootloader.bin \
    --main-ode   ~/fd-build/exact-main-ode/flippydrive-main-ode.bin \
    --fpkg       original/update.fpkg
# out.uf2 == recovery.uf2  (sha256 8c8fba53…)
```

Both modes need a stock `recovery.uf2` as the template, the same way `fpkg.py` needs the original
package: the parts this project does not build (FAT, manifest, DOLs) come from it unchanged.

## Flashing a modified bootloader

`pack` with a different `--bootloader` yields a flashable recovery image for that firmware while
keeping the FAT and DOLs intact. This is the vehicle for the `-DFD_ESP_EXTRACT` clean-bridge build
(see [ESP32.md](ESP32.md)): build that bootloader, pack it into a recovery image, flash over
BOOTSEL. Note the embedded `update.fpkg` in the FAT still carries the stock bootloader, so send the
extraction command before the firmware applies any pending update, or substitute a matching
`--fpkg` built from the modified images too.
