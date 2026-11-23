#!/usr/bin/env python3
"""Verify or regenerate the FlippyDrive recovery.uf2 (the BOOTSEL full-flash image).

recovery.uf2 (1.4.6) is an RP2040 UF2 of three flash segments:
  0x10000000  bootloader            (this project builds it byte-identical)
  0x10075000  a 4-entry FPK1 manifest header, then main-ode at 0x10080000 (byte-identical)
  0x10300000  a FAT filesystem holding the full 10-entry update.fpkg (at +0x9000) and the DOLs
Each firmware region matches our build exactly except its final 0x200 bytes: recovery leaves
them 0xFF (erased flash) where our images / the fpkg carry 0x00 trailing padding, so the tool
strips trailing zeros when substituting.

    python3 tools/recovery.py verify STOCK.uf2 \
        --bootloader bl.bin --main-ode mo.bin [--fpkg update.fpkg]
        -> report whether each region matches

    python3 tools/recovery.py pack STOCK.uf2 OUT.uf2 \
        [--bootloader bl.bin] [--main-ode mo.bin] [--fpkg update.fpkg]
        -> re-emit the UF2 with the given images substituted at their flash addresses.
           Blocks are relayed byte-for-byte and only payload bytes inside a substituted region
           are overwritten, so STOCK's exact framing (block order, numbering and the 0xFF/0x00
           structural padding the generator left) is preserved. With our matching builds this
           reproduces STOCK byte-for-byte; with a modified bootloader (e.g. the
           -DFD_ESP_EXTRACT clean-bridge build) it yields a flashable recovery image for it.

The FAT filesystem, its volume id/timestamps and the DOLs are relayed from STOCK, never rebuilt;
like fpkg.py --pack, only the parts this project reproduces are substituted.
"""
import argparse
import struct
import sys

UF2_MAGIC0, UF2_MAGIC1, UF2_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
BL_ADDR, MO_ADDR = 0x10000000, 0x10080000
FPKG_ADDR = 0x10309000  # 0x10300000 FAT base + 0x9000 (the update.fpkg's first data cluster)


def read_blocks(path):
    """Return the UF2 as a list of raw 512-byte bytearrays, validating each block's magic."""
    d = open(path, "rb").read()
    if len(d) % 512:
        sys.exit(f"{path}: not a multiple of 512 bytes")
    blocks = []
    for i in range(len(d) // 512):
        b = bytearray(d[i * 512:(i + 1) * 512])
        m0, m1 = struct.unpack_from("<2I", b, 0)
        end, = struct.unpack_from("<I", b, 508)
        if not (m0 == UF2_MAGIC0 and m1 == UF2_MAGIC1 and end == UF2_END):
            sys.exit(f"{path}: bad UF2 magic in block {i}")
        blocks.append(b)
    return blocks


def block_addr_psize(b):
    addr, psize = struct.unpack_from("<I", b, 12)[0], struct.unpack_from("<I", b, 16)[0]
    return addr, psize


def substitute(blocks, addr, image):
    """Overwrite payload bytes in [addr, addr+len(trimmed)) in place across the blocks. Trailing
    0x00 is trimmed so it maps to recovery's unwritten 0xFF. Returns (bytes_written, region_len)."""
    img = image.rstrip(b"\x00")
    end = addr + len(img)
    hit = 0
    for b in blocks:
        baddr, psize = block_addr_psize(b)
        for j in range(psize):
            fa = baddr + j
            if addr <= fa < end:
                b[32 + j] = img[fa - addr]
                hit += 1
    return hit, len(img)


def region_bytes(blocks, addr, length):
    """Read `length` flash bytes from `addr`, defaulting to 0xFF where no block covers them."""
    buf = bytearray(b"\xff" * length)
    for b in blocks:
        baddr, psize = block_addr_psize(b)
        for j in range(psize):
            fa = baddr + j
            if addr <= fa < addr + length:
                buf[fa - addr] = b[32 + j]
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["verify", "pack"])
    ap.add_argument("stock", help="stock recovery.uf2")
    ap.add_argument("out", nargs="?", help="output recovery.uf2 (pack mode)")
    ap.add_argument("--bootloader")
    ap.add_argument("--main-ode", dest="main_ode")
    ap.add_argument("--fpkg")
    args = ap.parse_args()
    blocks = read_blocks(args.stock)
    items = [(BL_ADDR, args.bootloader, "bootloader"), (MO_ADDR, args.main_ode, "main-ode"),
             (FPKG_ADDR, args.fpkg, "update.fpkg")]

    if args.mode == "verify":
        ok = True
        for addr, path, name in items:
            if not path:
                continue
            img = open(path, "rb").read().rstrip(b"\x00")
            match = region_bytes(blocks, addr, len(img)) == img
            ok &= match
            print(f"{name:12} @0x{addr:08x}: {'MATCH' if match else 'DIFFERS'} "
                  f"(0x{len(img):x} bytes after trimming trailing zeros)")
        sys.exit(0 if ok else 1)

    if not args.out:
        sys.exit("pack needs an output path")
    for addr, path, name in items:
        if path:
            hit, n = substitute(blocks, addr, open(path, "rb").read())
            print(f"{name:12} @0x{addr:08x}: substituted 0x{hit:x} of 0x{n:x} bytes")
    with open(args.out, "wb") as f:
        for b in blocks:
            f.write(b)
    print(f"wrote {args.out} ({len(blocks)} blocks)")


if __name__ == "__main__":
    main()
