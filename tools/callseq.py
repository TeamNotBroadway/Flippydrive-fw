#!/usr/bin/env python3
"""Check that a compiled function calls the same things in the same order as the original.

    callseq.py IMAGE BASE OBJ FUNC ADDR      (run in WSL; prints the edit distance)

Window scoring masks relocations, so it cannot see that two calls were swapped. This compares
the R_ARM_THM_CALL targets in OBJ's FUNC with the BL targets in the image from ADDR, naming
image addresses from syms/<image>.lib.tsv and syms/<image>.src.tsv. An unnamed target matches
anything. Flash-to-RAM veneers are followed to their RAM target.
"""
import os
import pathlib
import re
import subprocess
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symmatch import segments  # noqa: E402
from funcdiff import OBJDUMP  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def names(img):
    out = {}
    for kind in ("lib", "src"):
        p = ROOT / "syms" / f"{img}.{kind}.tsv"
        if p.exists():
            for line in p.read_text().splitlines():
                f = line.split("\t")
                out[int(f[0], 16) & ~1] = f[1]
    return out


def ours(obj, func):
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(os.path.dirname(os.path.dirname(OBJDUMP)), "hostlib"))
    out = subprocess.run([OBJDUMP, "-dr", obj], capture_output=True, text=True, env=env).stdout
    seq, size, inside = [], 0, False
    for l in out.splitlines():
        m = re.match(r"[0-9a-f]+ <(.+)>:", l)
        if m:
            inside = m.group(1) == func
            continue
        if not inside:
            continue
        m = re.search(r"R_ARM_THM_(?:CALL|JUMP24)\s+(\S+)", l)
        if m:
            seq.append(m.group(1).split("+")[0])
        m = re.match(r"\s+([0-9a-f]+):", l)
        if m:
            size = int(m.group(1), 16) + 4
    return seq, size


def theirs(segs, addr, size, known):
    def rd16(a):
        for base, data in segs:
            if base <= a < base + len(data) - 1:
                return struct.unpack_from("<H", data, a - base)[0]
        return None

    def rd32(a):
        lo, hi = rd16(a), rd16(a + 2)
        return None if lo is None or hi is None else lo | hi << 16

    seq = []
    a = addr
    while a < addr + size:
        h = rd16(a)
        if h is None:
            break
        h2 = rd16(a + 2) or 0
        if h & 0xf800 == 0xf000 and h2 & 0xd000 in (0xd000, 0x9000):  # BL, or B.W tail call
            s = (h >> 10) & 1
            imm = ((-s) << 24) | ((~((h2 >> 13) & 1) ^ s) & 1) << 23 | ((~((h2 >> 11) & 1) ^ s) & 1) << 22 \
                | (h & 0x3ff) << 12 | (h2 & 0x7ff) << 1
            t = a + 4 + imm
            if addr <= t < addr + size:   # a far branch within the function, not a call
                a += 4
                continue
            if t not in known:  # veneer: first RAM code pointer in its next 16 bytes
                for k in range(0, 16, 2):
                    w = rd32(t + k)
                    if w and 0x20000000 <= w < 0x20042000 and w & 1:
                        t = w & ~1
                        break
            seq.append(known.get(t, f"?{t:08x}"))
            a += 4
        elif h & 0xe000 == 0xe000 and h & 0x1800:
            a += 4
        else:
            a += 2
    return seq


def check(image_bin, base, obj, func, addr, img):
    known = names(img)
    segs = segments(pathlib.Path(image_bin).read_bytes(), base)
    a, size = ours(obj, func)
    nxt = min((k for k in known if addr < k < addr + size), default=addr + size)
    b = theirs(segs, addr, nxt - addr, known)
    return distance(a, b), a, b


def same(x, y):
    x = x.removeprefix("__wrap_")
    return y.startswith("?") or x in (n.removeprefix("__wrap_") for n in y.split("|"))


def distance(a, b):
    """Edit distance between call sequences; an unnamed image target matches anything."""
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (not same(x, y))))
        prev = cur
    return prev[-1]


def main():
    image_bin, base, obj, func, addr = sys.argv[1:6]
    img = "bootloader" if "bootloader" in image_bin else "main-ode"
    d, a, b = check(image_bin, int(base, 0), obj, func, int(addr, 0), img)
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "-"
        y = b[i] if i < len(b) else "-"
        print(f"{' ' if same(x, y) else '*'} {x:32} {y[:60]}")
    print("OK" if d == 0 else f"distance {d}")
    sys.exit(0 if d == 0 else 1)


if __name__ == "__main__":
    main()
