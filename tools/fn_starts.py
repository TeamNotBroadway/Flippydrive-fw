#!/usr/bin/env python3
"""List the function starts in an address range of an original image (run under WSL).

    python3 tools/fn_starts.py IMAGE LO HI      ->  address, extent, name (from lib/src/fatfs.tsv)

Starts are BL/B.W targets found inside Ghidra's functions (decomp/<image>/functions.tsv), plus
Ghidra's own starts and every address named in syms/. Linker veneers (16 bytes or less) are left
out. An extent runs to the next start, so it can include a literal pool or padding.
"""
import os, pathlib, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symmatch import segments
from libmap import BASE, ROOT, bl_targets


def names(img):
    out = {}
    for kind in ("lib", "src", "fatfs"):
        p = ROOT / "syms" / f"{img}.{kind}.tsv"
        for line in p.read_text().splitlines() if p.exists() else []:
            f = line.split("\t")
            if len(f) > 4 and f[4].startswith("near:"):
                continue
            out.setdefault(int(f[0], 16) & ~1, f[1])
    return out


def starts(img):
    segs = segments((ROOT / f"original/extracted/flippydrive-{img}.bin").read_bytes(), BASE[img])
    ghidra = [(int(l.split("\t")[0], 16), int(l.split("\t")[1]))
              for l in (ROOT / f"decomp/{img}/functions.tsv").read_text().splitlines()]
    s = {a for a, _ in ghidra} | set(names(img))
    for a, size in ghidra:
        s |= bl_targets(segs, a, a + size)
    return sorted(s)


def main():
    img, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    st = starts(img)
    nm = names(img)
    for i, a in enumerate(st):
        if lo <= a < hi:
            ext = (st[i + 1] if i + 1 < len(st) else hi) - a
            if ext > 16:
                print(f"0x{a:08x}\t{ext}\t{nm.get(a, '')}")


if __name__ == "__main__":
    main()
