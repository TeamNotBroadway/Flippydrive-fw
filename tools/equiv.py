#!/usr/bin/env python3
"""Classify each non-identical application function by how it differs from the original
(run under WSL after tools/check_all.sh).

    python3 tools/equiv.py [IMAGE ...] [--md]

For every function the exact build still copies from the original (gaps.tsv), compiles our C
and compares its instructions with the original's:

  register-only  same instruction sequence once registers and branch targets are erased
  reordered      same instructions (as a multiset) in a different order or block layout;
                 stack offsets and push/pop lists are erased for this test
  differs        neither; `match` is the share of instructions both sides have in common

Both tests keep mnemonics and immediates, so a changed constant, condition or operation shows
as `differs`. Also reported: the call sequence (same/differs, tools/callseq.py) and the
literal-pool words (same/differs; a word ours loads from an unresolved section such as a string
table or a static variable matches any flash or RAM pointer). Equivalence is not proven by any of these; `differs` means
the code may behave differently.
"""
import collections, functools, os, pathlib, re, struct, subprocess, sys, tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import permute as P
import callseq
from funcdiff import OBJDUMP, disasm
from symmatch import segments, elf_functions

ROOT = pathlib.Path(TOOLS).parent
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(os.path.dirname(os.path.dirname(OBJDUMP)), "hostlib"))
NM = OBJDUMP.replace("objdump", "nm")
REG = r"\b(?:r1[0-5]|r[0-9]|sl|fp|ip|sb|lr)\b"


def norm(mn, ops, loose):
    ops = re.sub(r"\s*[;@].*$", "", ops)
    if mn.startswith("b") and not mn.startswith(("bic", "bkpt")) or mn in ("cbz", "cbnz"):
        ops = re.sub(r"^.*?([0-9a-f]+)(\s*<.*>)?$", "T", ops) if mn not in ("bx", "blx") else ops
    if "[pc" in ops:
        ops = re.sub(r"\[pc.*", "LIT", ops)
    ops = re.sub(REG, "R", ops)
    if loose:
        if mn in ("push", "pop", "stmia", "ldmia"):
            ops = "{..}"
        ops = re.sub(r"\[sp, #\d+\]", "[sp,S]", ops)
        ops = re.sub(r"(sp, #)\d+", r"\1S", ops)
    return f"{mn} {ops}".strip()


def ours(obj, func, syms):
    """Instructions and literal-pool words of FUNC in OBJ. Pool words are read from the
    section bytes at the addresses its pc-relative loads name (objdump elides zero words)."""
    out = subprocess.run([OBJDUMP, "-dr", obj], capture_output=True, text=True, env=ENV).stdout
    ins, pool, rel, inside = [], set(), {}, False
    for l in out.splitlines():
        m = re.match(r"[0-9a-f]+ <(.+)>:", l)
        if m:
            inside = m.group(1) == func
            continue
        if not inside:
            continue
        m = re.match(r"\s+([0-9a-f]+): R_ARM_ABS32\s+(\S+)", l)
        if m:
            rel[int(m.group(1), 16)] = m.group(2)
            continue
        m = re.match(r"\s+[0-9a-f]+:\s+([0-9a-f]{8}|[0-9a-f]{4}(?: [0-9a-f]{4})?)\s+(\S+)\s*(.*)", l)
        if not m or m.group(2) == ".word":
            continue
        p = re.search(r"\[pc, #\d+\]\s*@ \(([0-9a-f]+)", m.group(3))
        if p:
            pool.add(int(p.group(1), 16))
        ins.append((m.group(2), m.group(3)))
    code = next((c for n, c, _ in elf_functions(pathlib.Path(obj).read_bytes()) if n == func), b"")
    lits = []
    for a in sorted(pool):
        w = struct.unpack_from("<I", code, a)[0] if a + 4 <= len(code) else 0
        if a in rel:
            s = syms.get(rel[a].split("+")[0])
            w = (s + w) if s is not None and not rel[a].startswith(".") else "SECTION"
        lits.append(w)
    return ins, lits


def theirs(segs, addr, end):
    code = b""
    for base, data in segs:
        if base <= addr < base + len(data):
            code = data[addr - base:end - base]
    lines = disasm(code, addr)
    pool = set()
    for l in lines:
        m = re.search(r"\[pc, #\d+\]\s*@ \((?:0x)?([0-9a-f]+)\)", l)
        if m:
            pool.add(int(m.group(1), 16))
    ins, lits = [], []
    for l in lines:
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{4}(?: [0-9a-f]{4})?)\s+(\S+)\s*(.*)", l)
        if not m:
            continue
        a = int(m.group(1), 16)
        if any(p <= a < p + 4 for p in pool):
            if a in pool:
                off = a - addr
                lits.append(struct.unpack_from("<I", code, off)[0] if off + 4 <= len(code) else None)
            continue
        if m.group(2) == "0000" and any(a < p <= a + 2 for p in pool):
            continue  # pool alignment
        ins.append((m.group(3), m.group(4)))
    return ins, lits


def lits_unmatched(a, b):
    """Words of ours with no counterpart in the original, and the original's left over."""
    b = collections.Counter(b)
    miss = []
    for x in sorted(a, key=lambda x: x == "SECTION"):  # resolved words first
        if x == "SECTION":
            k = next((v for v in b if v and 0x10000000 <= v < 0x20042000 and b[v]), None)
        else:
            k = x if b[x] else None
        if k is None:
            miss.append(x)
        else:
            b[k] -= 1
    return miss, [v for v, n in b.items() for _ in range(n)]


@functools.lru_cache(None)
def source_of(img, func):
    pat = re.compile(r"^[A-Za-z_][^;\n]*\b%s\b\)?\s*\([^;\n]*$" % re.escape(func), re.M)
    for p in sorted((ROOT / img).glob("*.c")):
        if pat.search(p.read_text(errors="replace")):
            return p
    return None


def main():
    imgs = [a for a in sys.argv[1:] if not a.startswith("--")] or ["bootloader", "main-ode"]
    md = "--md" in sys.argv
    if md:
        print("| Image | Function | Address | Original bytes | Instructions ours/orig | Class | Match | Calls | Literals |")
        print("|---|---|---|---:|---:|---|---:|---|---|")
    for img in imgs:
        d = pathlib.Path.home() / f"fd-build/exact-{img}"
        elf = d / "exact.elf"
        nm = subprocess.run([NM, "-n", str(elf)], capture_output=True, text=True, env=ENV).stdout
        syms, starts = {}, set()
        for l in nm.splitlines():
            p = l.split()
            if len(p) == 3:
                syms.setdefault(p[2], int(p[0], 16))
                if p[1] in "tTwW" and not p[2].startswith(("$", ".L")):
                    starts.add(int(p[0], 16) & ~1)
        known = callseq.names(img)
        byname = {v: k for k, v in known.items()}
        exact = {r.split("\t")[3].rsplit(".", 1)[-1] for r in open(d / "sections.tsv")}  # placed = identical
        for n, a in syms.items():
            if n not in byname and (ROOT / img).is_dir():
                byname[n] = a & ~1
        funcs, other = [], {}
        for l in open(d / "gaps.tsv"):
            c = l.rstrip("\n").split("\t")
            if len(c) > 2 and c[2] in byname and c[2] not in exact and "|" not in c[2] \
                    and source_of(img, c[2]):
                if c[2] not in funcs:
                    funcs.append(c[2])
            elif len(c) > 1:  # gaps.tsv names the preceding symbol, so these are other code
                k = c[2] if len(c) > 2 and c[2] else "(unnamed)"
                other[k] = other.get(k, 0) + int(c[1])
        binp = ROOT / f"original/extracted/flippydrive-{img}.bin"
        segs = segments(binp.read_bytes(), int(P.BASE[img], 0))
        bounds = sorted(starts | set(known))
        for f in funcs:
            addr = byname[f]
            end = next(b for b in bounds if b > addr)
            src = source_of(img, f)
            if src is None:
                print(f"{img} {f}: no source defines it")
                continue
            with tempfile.TemporaryDirectory() as td:
                o = os.path.join(td, "f.o")
                r = subprocess.run(f"{P.CC} -I{src.parent} {P.build_flags(img, str(src))} -c {src} -o {o}",
                                   shell=True, capture_output=True, text=True)
                if r.returncode:
                    print(f"{img} {f}: compile failed")
                    continue
                a_ins, a_lit = ours(o, f, syms)
                dist, _, _ = callseq.check(str(binp), int(P.BASE[img], 0), o, f, addr, img)
            b_ins, b_lit = theirs(segs, addr, end)
            na = [norm(m, x, False) for m, x in a_ins]
            nb = [norm(m, x, False) for m, x in b_ins]
            la = collections.Counter(norm(m, x, True) for m, x in a_ins)
            lb = collections.Counter(norm(m, x, True) for m, x in b_ins)
            if na == nb:
                cls = "register-only"
            elif la == lb:
                cls = "reordered"
            else:
                cls = "differs"
            match = sum((la & lb).values()) / max(sum(la.values()), sum(lb.values()), 1)
            calls = "same" if dist == 0 else f"differs ({dist})"
            miss, left = lits_unmatched(a_lit, b_lit)
            lit = "same" if not miss and not left else \
                "differs (ours " + " ".join(x if isinstance(x, str) else f"{x:#x}" for x in miss) + \
                "; orig " + " ".join(f"{x:#x}" if x is not None else "?" for x in left) + ")"
            row = (img, f, f"{addr:#x}", end - addr, f"{len(a_ins)}/{len(b_ins)}", cls, f"{100 * match:.0f}%", calls, lit)
            if md:
                print("| " + " | ".join(f"`{x}`" if i == 1 else str(x) for i, x in enumerate(row)) + " |")
            else:
                print("\t".join(map(str, row)), flush=True)
        if not md:
            print(f"{img}: other copied bytes, by preceding symbol: " +
                  ", ".join(f"{k} {v}" for k, v in sorted(other.items(), key=lambda t: -t[1])))


if __name__ == "__main__":
    main()
