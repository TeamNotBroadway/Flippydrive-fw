#!/usr/bin/env python3
"""Read the ESP32's eFuse state and flash, to recover the decrypted firmware FROM A DEVICE.

The three ESP32 images in update.fpkg are ESP32 hardware flash encryption (docs/ESP32.md); the
key is not in any public artifact. The one route to the plaintext firmware is on-device: if the
board's flash encryption is in DEVELOPMENT mode, the ROM download loader decrypts flash reads
transparently, so esptool read_flash returns plaintext. This script runs that check and, if it
looks possible, dumps and classifies the flash. It is READ-ONLY: it never burns an eFuse and
never writes or erases flash.

    python3 tools/esp32_dump.py --port COM5                 (direct USB-serial wiring)
    python3 tools/esp32_dump.py --port COM5 --no-reset      (ESP32 already in download mode)

Needs esptool: `pip install esptool` (provides esptool + espefuse). Untested here — no board.

How to reach the ESP32's ROM download mode
------------------------------------------
The ESP32 boot (IO0) and reset (EN) lines are on the FlippyDrive's I/O expander, driven by the
RP2040 (`bootloader/esp_port.c` loader_port_enter_bootloader), NOT by serial DTR/RTS. So:

- Direct wiring (reliable): tap the ESP32 chip's TXD0/RXD0/GND with a USB-serial adapter, and
  its IO0/EN so the adapter's DTR/RTS can auto-reset it (standard esptool circuit). Then omit
  --no-reset. This bypasses the RP2040 entirely.
- FlippyDrive bridge: get the ESP32 into download mode through the FlippyDrive's own flow
  (it already pulses IO0/EN during an ESP32 update), then run the FlippyDrive's USB-UART bridge
  and point --port at that CDC port with --no-reset. esptool cannot drive IO0/EN over the
  bridge itself, so the board must already be in download mode.
"""
import argparse
import pathlib
import re
import subprocess
import sys

# ESP32 flash regions this project cares about (offset, length, name), from docs/ESP32.md.
REGIONS = [(0x00000, 0x0ac00, "esp_bootldr"), (0x0f000, 0x00e00, "esp_parttab"),
           (0x60000, 0xe8800, "esp_ode")]


def tool(name):
    """Return the argv prefix for esptool/espefuse (module form, then script form)."""
    for argv in ([sys.executable, "-m", name], [f"{name}.py"], [name]):
        try:
            subprocess.run(argv + ["--help"], capture_output=True, timeout=30)
            return argv
        except (OSError, subprocess.SubprocessError):
            continue
    sys.exit(f"{name} not found. Install with: pip install esptool")


def run(argv, **kw):
    print("  $", " ".join(str(a) for a in argv))
    r = subprocess.run(argv, text=True, capture_output=True, **kw)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.stderr.write(r.stderr)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port (e.g. COM5 or /dev/ttyACM0)")
    ap.add_argument("--baud", default="115200")
    ap.add_argument("--chip", default="esp32")
    ap.add_argument("--no-reset", action="store_true",
                    help="board is already in download mode; do not toggle DTR/RTS")
    ap.add_argument("--size", default="0x400000", help="flash size to read (default 4 MB)")
    ap.add_argument("--out", default="esp32_dump")
    args = ap.parse_args()
    out = pathlib.Path(args.out); out.mkdir(parents=True, exist_ok=True)
    before = ["--before", "no_reset"] if args.no_reset else []
    common = ["--chip", args.chip, "--port", args.port, "--baud", args.baud] + before + ["--after", "no_reset"]

    espefuse, esptool = tool("espefuse"), tool("esptool")

    # 0. chip and silicon revision ----------------------------------------------------------
    # esptool's connection banner reports the exact part and revision, e.g.
    #   "Chip is ESP32-D0WD-V3 (revision v3.0)". The revision decides whether the release-mode
    # hardware key-extraction fallback (docs/ESP32.md) is even feasible.
    print("\n[0] Identifying the chip ...")
    r = run(esptool + common + ["flash_id"])
    banner = r.stdout
    (out / "chip_id.txt").write_text(banner)
    chip = next((l.strip() for l in banner.splitlines() if l.strip().startswith("Chip is")), None)
    revm = re.search(r"revision\s*v?(\d+)(?:\.(\d+))?", banner, re.IGNORECASE)
    rev = int(revm.group(1)) if revm else None
    print("  " + (chip or "chip not reported; see chip_id.txt"))
    if rev is not None:
        note = ("v0/v1 — vulnerable to published voltage-glitch key extraction (LimitedResults)"
                if rev <= 1 else
                "v2 — intermediate; some fault-injection mitigations" if rev == 2 else
                "v3+ — hardened; no public key-extraction attack")
        print(f"  Silicon revision v{rev}: {note}")

    # 1. eFuse state ------------------------------------------------------------------------
    print("\n[1] Reading eFuse summary ...")
    r = run(espefuse + ["--port", args.port] + (before or []) + ["summary"])
    summ = r.stdout
    (out / "efuse_summary.txt").write_text(summ)
    if r.returncode:
        sys.exit("espefuse failed. Check the port, wiring, and that the ESP32 is in download mode.")

    def field(name):
        m = re.search(rf"{re.escape(name)}\b.*?=?\s*([A-Za-z0-9_/ ]+?)(?:\s*R/W|\s*=|\s*\(|$)",
                      summ, re.MULTILINE)
        return (m.group(1).strip() if m else "?")

    enc_mode = next((l.split(":", 1)[1].strip() for l in summ.splitlines()
                     if "Flash encryption mode" in l), None)
    dl_decrypt = field("DISABLE_DL_DECRYPT")
    print("\n  Flash encryption mode:", enc_mode or "(not reported; see efuse_summary.txt)")
    for k in ("FLASH_CRYPT_CNT", "FLASH_CRYPT_CONFIG", "DISABLE_DL_DECRYPT",
              "DISABLE_DL_ENCRYPT", "DISABLE_DL_CACHE", "ABS_DONE_0", "ABS_DONE_1",
              "JTAG_DISABLE", "UART_DOWNLOAD_DIS"):
        if k in summ:
            print(f"  {k}: {field(k)}")

    plaintext_possible = ("DEVELOPMENT" in (enc_mode or "")) or dl_decrypt in ("0", "False", "0b0")
    print("\n  Verdict:", "download-mode reads should be DECRYPTED (plaintext) — proceeding."
          if plaintext_possible else
          "download-mode reads will be CIPHERTEXT (release mode / DL decrypt disabled).\n"
          "  read_flash below will return the same encrypted bytes as update.fpkg; see "
          "docs/ESP32.md for the release-mode options.")

    # 2. read flash -------------------------------------------------------------------------
    print(f"\n[2] Reading 0..{args.size} of flash ...")
    full = out / "flash.bin"
    r = run(esptool + common + ["read_flash", "0", args.size, str(full)])
    if r.returncode or not full.exists():
        sys.exit("read_flash failed.")

    # 3. classify the regions ---------------------------------------------------------------
    print("\n[3] Classifying the regions in the dump:")
    data = full.read_bytes()
    import math
    import collections
    for off, ln, name in REGIONS:
        seg = data[off:off + ln]
        if len(seg) < ln:
            print(f"  {name:12} @0x{off:05x}: dump too short"); continue
        (out / f"{name}.bin").write_bytes(seg)
        c = collections.Counter(seg)
        ent = -sum((n / len(seg)) * math.log2(n / len(seg)) for n in c.values())
        e9 = seg[0] == 0xE9
        print(f"  {name:12} @0x{off:05x}: entropy {ent:.2f}/8  first4={seg[:4].hex()}  "
              + ("PLAINTEXT (0xE9 image) — decrypted firmware recovered" if e9
                 else "plaintext" if ent < 6.5 else "still encrypted"))

    print(f"\nDone. Files in {out}/. If the regions read as plaintext (0xE9), they are the "
          "decrypted ESP32 firmware; feed them to Ghidra/objdump as Xtensa images.")


if __name__ == "__main__":
    main()
