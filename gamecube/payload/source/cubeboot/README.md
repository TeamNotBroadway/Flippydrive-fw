# cubeboot sources in the payload

From OffBroadway/cubeboot (GPLv2, `LICENSE`). Each file is taken at the revision whose
compiled code matches the payload (devkitPPC r47.1, `-O2 -ffunction-sections
-fdata-sections`, libogc2 70f80d8 headers), then edited where the payload differs.

| File | cubeboot revision and path | Payload |
|---|---|---|
| `flippy_sync.c`, `flippy_sync.h`, `ipc.h` | e0b6699f `cubeboot/source/` | 0x800059dc-0x800061e4 |
| `gc_dvd.c`, `time.h` | eb14628b `patches/source/` | 0x80006350-0x80006364 |
| `sidestep.c`, `sidestep.h` | 93cfca24 `cubeboot/source/boot/` | 0x80008a68-0x80009074 |
| `ssaram.c`, `ssaram.h` | 0c4e864e `cubeboot/source/boot/` | 0x80009074-0x80009484 |

Edits:

- `flippy_sync.c`: adds `dvd_bootloader_status_ext` (DI command 0xB3, 0x160-byte response),
  which no public source has; the name is ours. `dvd_custom_status` returns 1 on error, not -1.
- `print.h`: replaces cubeboot's; `iprintf` compiles to nothing, since the payload contains
  none of `flippy_sync.c`'s messages.
- `sidestep.h`: declares `DOLMinMax`, which `main.c` calls.
- `sidestep.c`: `DOLMinMax` is global, returns the size before the BSS plus 0x120 and drops the
  0x20000 pad; `ARAMRun` is static, so it is inlined into `DOLtoARAM`.
