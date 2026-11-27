#!/usr/bin/env python3
"""Ghidra's decompile of one function, with addresses replaced by our names (run under WSL).

    python3 tools/ghidra_fn.py IMAGE ADDR

Prints FUN_<ADDR> from decomp/IMAGE/app.c. FUN_xxx calls become our function names
(syms/IMAGE.*.tsv and the exact build's symbols). DAT_xxx literal-pool words are read from the
exact build (~/fd-build/exact-IMAGE/exact.elf, byte-identical to the original): a word that
points at one of our symbols becomes `&name` or `&name+off`, so `*DAT_x` reads `*&name`; other
words become their hex value. Use it as the starting point for rewriting a function: Ghidra's
block order and conditions follow the original's code.
"""
import os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TC = "/mnt/c/Tools/arch-arm-none-eabi-14.2.0"


def sections(elf):
    d = open(elf, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2e)
    out = []
    for i in range(shnum):
        _, typ, _, addr, off, size = struct.unpack_from("<IIIIII", d, shoff + i * shentsize)
        if typ == 1 and addr:  # PROGBITS with an address
            out.append((addr, off, size))
    return d, out


def word(d, secs, a):
    for addr, off, size in secs:
        if addr <= a and a + 4 <= addr + size:
            return struct.unpack_from("<I", d, off + a - addr)[0]
    return None


def symbols(elf):
    out = subprocess.run([f"{TC}/bin/arm-none-eabi-nm", "-n", "-S", elf], capture_output=True,
                         text=True).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in "bBdDrRtTwW" and not p[3].startswith(("$", "__gap", ".L")):
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    return syms


def main():
    img, addr = sys.argv[1], int(sys.argv[2], 16)
    text = open(os.path.join(ROOT, f"decomp/{img}/app.c"), errors="replace").read()
    m = re.search(r"^[^\n;]*\bFUN_%08x\([^;{]*\)\s*\{" % addr, text, re.M)
    if not m:
        sys.exit(f"FUN_{addr:08x} not in decomp/{img}/app.c")
    i, depth = text.index("{", m.start()), 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            break
    body = text[m.start():j + 1]

    names = {}
    for f in ("src", "lib", "fatfs"):
        p = os.path.join(ROOT, f"syms/{img}.{f}.tsv")
        if os.path.exists(p):
            for l in open(p):
                c = l.rstrip("\n").split("\t")
                if len(c) > 1 and c[0].startswith("0x"):
                    names.setdefault(int(c[0], 16) & ~1, c[1])
    elf = os.path.expanduser(f"~/fd-build/exact-{img}/exact.elf")
    d, secs = sections(elf)
    syms = symbols(elf)
    for a, _, n in syms:
        names.setdefault(a & ~1, n)

    def sym_at(v):
        best = None
        for a, s, n in syms:
            if a <= v < a + max(s, 1):
                best = (a, n)
        return best

    def fun(mm):
        return names.get(int(mm.group(1), 16), mm.group(0))

    def dat(mm):
        a = int(mm.group(1), 16)
        v = word(d, secs, a) if a % 4 == 0 else None
        if v is None:
            s = sym_at(a)
            return f"{s[1]}+{a - s[0]:#x}" if s and a != s[0] else (s[1] if s else mm.group(0))
        s = sym_at(v & ~1) if v >= 0x10000000 else None
        if s:
            return f"&{s[1]}" + (f"+{v - s[0]:#x}" if v != s[0] else "")
        return f"{v:#x}"

    body = re.sub(r"\bFUN_([0-9a-f]{8})\b", fun, body)
    def addr_of(mm):  # &DAT_x is the address x itself, not a pool word
        a = int(mm.group(1), 16)
        s = sym_at(a) if a >= 0x10000000 else None
        return f"&{s[1]}" + (f"+{a - s[0]:#x}" if a != s[0] else "") if s else f"{a:#x}"

    body = re.sub(r"&DAT_([0-9a-f]{8})\b", addr_of, body)
    body = re.sub(r"\bDAT_([0-9a-f]{8})\b", dat, body)
    body = re.sub(r"\*&([A-Za-z_]\w*)\b(?!\+)", r"\1", body)
    print(body)


if __name__ == "__main__":
    main()
