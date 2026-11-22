#!/usr/bin/env python3
"""Compare the constants a function loads from its literal pool with the original's.

    litcheck.py IMAGE BASE OBJ FUNC ADDR      (run in WSL)

Window scoring masks relocations, and a wrong constant in a pool reads as a near match.
This collects the unrelocated words our FUNC loads with pc-relative LDR and reports any that
the original's pc-relative loads from ADDR do not also load.
"""
import collections
import os
import pathlib
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symmatch import segments  # noqa: E402
from funcdiff import OBJDUMP  # noqa: E402
import callseq  # noqa: E402

LDR = re.compile(r"\bldr\s+r\d+, \[pc, #(\d+)\]")


def ours(obj, func):
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(os.path.dirname(os.path.dirname(OBJDUMP)), "hostlib"))
    out = subprocess.run([OBJDUMP, "-dr", obj], capture_output=True, text=True, env=env).stdout
    half, loads, relocs, inside = {}, [], set(), False
    for l in out.splitlines():
        m = re.match(r"[0-9a-f]+ <(.+)>:", l)
        if m:
            inside = m.group(1) == func
            continue
        if not inside:
            continue
        m = re.match(r"\s+([0-9a-f]+):\s+R_ARM_\w+", l)
        if m:
            relocs.add(int(m.group(1), 16))
            continue
        m = re.match(r"\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+\.word", l)
        if m:
            a, w = int(m.group(1), 16), int(m.group(2), 16)
            half[a], half[a + 2] = w & 0xffff, w >> 16
            continue
        m = re.match(r"\s+([0-9a-f]+):\s+([0-9a-f]{4})(?: ([0-9a-f]{4}))?\s", l)
        if not m:
            continue
        a = int(m.group(1), 16)
        half[a] = int(m.group(2), 16)
        if m.group(3):
            half[a + 2] = int(m.group(3), 16)
        m2 = LDR.search(l)
        if m2:
            loads.append(((a + 4) & ~3) + int(m2.group(1)))
    vals = []
    for t in loads:
        if t in relocs or t not in half or t + 2 not in half:
            continue
        vals.append(half[t] | half[t + 2] << 16)
    return vals, (max(half) + 2 if half else 0)


def theirs(segs, addr, size):
    def rd(a, n):
        for b, d in segs:
            if b <= a < b + len(d) - n + 1:
                return d[a - b:a - b + n]
        return None

    vals = []
    a = addr
    while a < addr + size:
        h = rd(a, 2)
        if h is None:
            break
        h = struct.unpack("<H", h)[0]
        if h & 0xf800 == 0x4800:  # LDR rX, [pc, #imm8*4]
            t = ((a + 4) & ~3) + (h & 0xff) * 4
            w = rd(t, 4)
            if w:
                vals.append(struct.unpack("<I", w)[0])
            a += 2
        elif h & 0xe000 == 0xe000 and h & 0x1800:
            a += 4
        else:
            a += 2
    return vals


def check(image_bin, base, obj, func, addr):
    segs = segments(pathlib.Path(image_bin).read_bytes(), base)
    a, size = ours(obj, func)
    b = theirs(segs, addr, size)
    missing = collections.Counter(a) - collections.Counter(b)
    return sorted(missing)


def main():
    image_bin, base, obj, func, addr = sys.argv[1:6]
    miss = check(image_bin, int(base, 0), obj, func, int(addr, 0))
    print("OK" if not miss else "constants not in the original: " + " ".join(f"0x{v:x}" for v in miss))
    sys.exit(1 if miss else 0)


if __name__ == "__main__":
    main()
