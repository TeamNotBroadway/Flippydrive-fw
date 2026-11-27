#!/usr/bin/env python3
"""Clean-bridge bootloader: the original image with the USB<->ESP32 bridge passing every byte
through unmodified (run under WSL or Windows Python).

    python3 tools/patch_bootloader.py OUTDIR

Writes OUTDIR/flippydrive-bootloader.bin and OUTDIR/update.fpkg (the original package with
that entry replaced; its header MD5 updated by tools/fpkg.py). Every byte not listed in PATCHES
is the original's, so this needs no decompiled C; the run checks that only those bytes differ.

The stock bridge (USB-CDC command 0x12, usb_uart_bridge at 0x200000e4) already puts the ESP32
into ROM download mode and bridges USB<->UART, but sends '\r' before every byte equal to
bridge_escape, a zero-initialised int16 nothing writes: so every 0x00 byte from the host gets a
'\r' in front, which breaks esptool's binary SLIP framing. Patch: the `beq` that takes the
'\r' path becomes a `nop`. The bridge still sends its 0x06 acknowledgement byte first.
"""
import os, pathlib, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symmatch import segments  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
ORIG = ROOT / "original/extracted/flippydrive-bootloader.bin"
PKG = ROOT / "original/update.fpkg"

# (RAM address, original bytes, new bytes, what it does). RAM code is copied from flash at
# boot; the flash copy is found by searching for the surrounding original bytes.
PATCHES = [
    (0x20000198, bytes.fromhex("15d0"), bytes.fromhex("c046"),
     "usb_uart_bridge: `beq 0x200001c6` (byte == bridge_escape -> send '\\r') -> `nop`"),
]
CONTEXT = 16  # bytes either side used to locate the flash copy


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "permute_out/clean_bridge")
    out.mkdir(parents=True, exist_ok=True)
    img = bytearray(ORIG.read_bytes())
    segs = segments(bytes(img), 0x10000000)
    changed = []
    for ram, old, new, what in PATCHES:
        base, data = next((b, d) for b, d in segs if b <= ram < b + len(d))
        k = ram - base
        assert data[k:k + len(old)] == old, f"{ram:#x}: expected {old.hex()}, found {data[k:k + len(old)].hex()}"
        ctx = data[k - CONTEXT:k + len(old) + CONTEXT]
        hits = [i for i in range(len(img)) if img[i:i + len(ctx)] == ctx]
        assert len(hits) == 1, f"{ram:#x}: context found {len(hits)} times in the flash image"
        off = hits[0] + CONTEXT
        img[off:off + len(old)] = new
        changed.append((off, len(old), ram, what))
        print(f"flash 0x{0x10000000 + off:08x} (RAM 0x{ram:08x}): {old.hex()} -> {new.hex()}  {what}")
    orig = ORIG.read_bytes()
    diff = [i for i in range(len(orig)) if orig[i] != img[i]]
    allowed = {o + j for o, n, _, _ in changed for j in range(n)}
    assert len(img) == len(orig) and set(diff) <= allowed, "bytes outside the patches differ"
    bin_out = out / "flippydrive-bootloader.bin"
    bin_out.write_bytes(img)
    subprocess.run([sys.executable, str(ROOT / "tools/fpkg.py"), "--pack", str(PKG), str(out / "update.fpkg"),
                    str(bin_out)], check=True)
    print(f"{len(diff)} bytes differ from the original; wrote {bin_out} and {out / 'update.fpkg'}")


if __name__ == "__main__":
    main()
