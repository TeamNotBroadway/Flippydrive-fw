# Third-party entries in update.fpkg

`update.fpkg` 1.4.6 comes from the
[OffBroadway/flippydrive-assets 1.4.6 release](https://github.com/OffBroadway/flippydrive-assets/releases/tag/1.4.6).
None of the entries below is published separately for 1.4.6. The sources were found from
strings in the entries (after decompressing them where needed) and the GitHub API. None has
been rebuilt and compared, so "source" means "identified", not "matched".

| Entry | Header version | Identified as | Source | Status |
|---|---|---|---|---|
| `swiss-gc.dol` | 0.0.0 | Swiss v0.6, xz-compressed DOL (stream at 0x3700). Built-in strings: GIT commit `40a65cb3`, revision `1867-fd-4` | [OffBroadway/swiss-gc@40a65cb3](https://github.com/OffBroadway/swiss-gc/commit/40a65cb37d1efd9271500d3e0566dec0bbd77930) ("disable to autoload dol feature"). The commit is not on any current branch of the fork | source identified |
| `stub.bin` | 0.0.0 | Swiss stub, `STUBHAXX` marker | `cube/patches/stub/` in OffBroadway/swiss-gc (code search hit on the default branch; the revision is not pinned) | source identified, revision unknown |
| `cubeboot.dol` | 0.0.0 | cubeboot entry stub plus gzip payload (member at 0x16c0); payload says `cubeboot rc15`, newlib 4.5.0.20241231 | [OffBroadway/cubeboot branch beta-rc15](https://github.com/OffBroadway/cubeboot/tree/beta-rc15), head 873893b4. `docs/PLAN.md` already matched 20 payload functions against this version | source identified |
| `backup.dol` | 0.0.0 | FlippyDrive disc backup utility: "Dumping Disc...", "dump in fallback mode", "dump in fast mode (DISABLED)" ([docs page](https://github.com/OffBroadway/flippydrive/blob/main/source/backup.rst)) | not found. OffBroadway/cleanrip (a fork of emukidid/cleanrip) has none of these strings | no public source |
| `remote.dol` | 0.0.0 | "Switching to Remote...", "Remote Access complete...", newlib 4.5.0.20241231 | not found in OffBroadway's repositories or by GitHub code search | no public source |
| `esp_bootldr.bin`, `esp_parttab.bin`, `esp_ode.bin` | 0.2.0, 0.2.1, 0.2.2 | ESP32 bootloader, partition table and application; encrypted (no 0xE9 image magic, incompressible) | none public | opaque |

The bootloader passes the ESP32 entries to the ESP32 unchanged: `flash_esp32`
(`bootloader/app_c.c`) reads 0x200-byte blocks from the package and writes them with
`esp_flash_write`, with no transformation on the RP2040 side.
