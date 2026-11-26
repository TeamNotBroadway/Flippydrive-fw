#!/usr/bin/env python3
"""Locate a library's functions in an original image by call graph (run under WSL).

    python3 tools/libmap.py IMAGE OBJ... [--prefix P] [--out FILE]

Function starts are every BL/B.W target found inside Ghidra's functions
(decomp/<image>/functions.tsv) plus those starts; a function's extent runs to the next start.
Names come from syms/<image>.{lib,src}.tsv, without the `near:` guesses. From each named
function that one of OBJ... defines, our call sequence (R_ARM_THM_CALL/JUMP24 relocations) is
aligned with the original's BL targets; an unnamed target paired with one of our callees names
that address. This repeats until nothing changes.

Prints, for every function OBJ... defines: address (or -), original extent, our size, the
function it was found from, and conflicts. With --prefix only names in objects whose path
contains P are printed; --out writes them as a lib.tsv fragment.
"""
import argparse, os, pathlib, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import callseq
from symmatch import segments
from funcdiff import OBJDUMP

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = {"bootloader": 0x10000000, "main-ode": 0x10080000}
# pico_divider enters the quotient-only and the divmod functions at one address.
# Alignments with more edits than this share of their calls name nothing (cmd_ipc, 117/291 =
# 0.402, pairs neighbours wrongly; mount_fat, 4/10, is right).
MAX_EDITS = 0.4
ALIASES = {"__aeabi_uidiv": "__aeabi_uidivmod", "__aeabi_idiv": "__aeabi_idivmod"}
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(os.path.dirname(os.path.dirname(OBJDUMP)), "hostlib"))


def our_functions(obj):
    """{name: (call sequence, size)} for every function in OBJ."""
    out = subprocess.run([OBJDUMP, "-dr", obj], capture_output=True, text=True, env=ENV).stdout
    funcs, cur = {}, None
    for l in out.splitlines():
        m = re.match(r"[0-9a-f]+ <(.+)>:", l)
        if m:
            cur = m.group(1)
            if cur.startswith("."):          # section symbol, not a function
                cur = None
            else:
                funcs[cur] = [[], 0]
            continue
        if cur is None:
            continue
        m = re.search(r"R_ARM_THM_(?:CALL|JUMP24)\s+(\S+)", l)
        if m:
            callee = m.group(1).split("+")[0].removeprefix(".text.")
            funcs[cur][0].append(ALIASES.get(callee, callee))
        m = re.match(r"\s+([0-9a-f]+):", l)
        if m:
            funcs[cur][1] = int(m.group(1), 16) + 4
    return {k: tuple(v) for k, v in funcs.items()}


def known_names(img):
    out = {}
    for kind in ("lib", "src"):
        p = ROOT / "syms" / f"{img}.{kind}.tsv"
        for line in p.read_text().splitlines() if p.exists() else []:
            f = line.split("\t")
            if len(f) > 4 and f[4].startswith("near:"):
                continue
            for n in f[1].split("|"):
                out.setdefault(n.removeprefix("__wrap_"), int(f[0], 16) & ~1)
    for alias, name in ALIASES.items():
        if name in out:
            out.setdefault(alias, out[name])
    return out


def bl_targets(segs, lo, hi):
    out = set()
    rd = lambda a: next((int.from_bytes(d[a - b:a - b + 2], "little") for b, d in segs
                         if b <= a < b + len(d) - 1), None)
    a = lo
    while a < hi:
        h, h2 = rd(a), rd(a + 2) or 0
        if h is None:
            break
        if h & 0xf800 == 0xf000 and h2 & 0xd000 in (0xd000, 0x9000):
            s = (h >> 10) & 1
            imm = ((-s) << 24) | ((~((h2 >> 13) & 1) ^ s) & 1) << 23 | ((~((h2 >> 11) & 1) ^ s) & 1) << 22 \
                | (h & 0x3ff) << 12 | (h2 & 0x7ff) << 1
            out.add(a + 4 + imm)
            a += 4
        elif h & 0xe000 == 0xe000 and h & 0x1800:
            a += 4
        else:
            a += 2
    return out


def align(ours, theirs, addr_of, name_at):
    """Pairs (our callee, image address) from an edit-distance alignment.

    A pair costs 0 when the image address already carries that name, or when neither side is
    named yet; otherwise 1.
    """
    def cost(x, t):
        if addr_of.get(x) == t:
            return 0
        n = name_at.get(t)
        if n:
            return 0 if x in n else 1
        return 0 if x not in addr_of else 1

    n, m = len(ours), len(theirs)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d[i][j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost(ours[i - 1], theirs[j - 1]))
    path, i, j = [], n, m          # path entries: (our callee, image address, kind)
    while i and j:
        c = cost(ours[i - 1], theirs[j - 1])
        if d[i][j] == d[i - 1][j - 1] + c:
            x, t = ours[i - 1], theirs[j - 1]
            kind = "edit" if c else ("named" if t in name_at or x in addr_of else "new")
            path.append((x, t, kind))
            i, j = i - 1, j - 1
        elif d[i][j] == d[i - 1][j] + 1:
            path.append((ours[i - 1], None, "edit")); i -= 1
        else:
            path.append((None, theirs[j - 1], "edit")); j -= 1
    path.reverse()
    if d[n][m] == 0:
        return [(x, t) for x, t, k in path if k == "new"], 0
    pairs = [(x, t) for x, t, k in path if k == "new"]
    return pairs, d[n][m]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("img"); ap.add_argument("objs", nargs="+")
    ap.add_argument("--prefix"); ap.add_argument("--out")
    ap.add_argument("--debug", action="store_true", help="print each aligned call sequence")
    a = ap.parse_args()
    img = a.img
    segs = segments((ROOT / f"original/extracted/flippydrive-{img}.bin").read_bytes(), BASE[img])

    ghidra = [(int(l.split("\t")[0], 16), int(l.split("\t")[1]))
              for l in (ROOT / f"decomp/{img}/functions.tsv").read_text().splitlines()]
    starts = {s for s, _ in ghidra}
    for s, size in ghidra:
        starts |= bl_targets(segs, s, s + size)
    known = known_names(img)
    calls = set()
    for s, size in ghidra:
        calls |= bl_targets(segs, s, s + size)
    calls |= set(known.values())
    starts |= calls
    order = sorted(starts)
    nxt = {s: order[i + 1] if i + 1 < len(order) else s + 0x10000 for i, s in enumerate(order)}
    corder = sorted(calls)          # call targets only: Ghidra splits some functions wrongly
    cnxt = {s: corder[i + 1] if i + 1 < len(corder) else s + 0x10000 for i, s in enumerate(corder)}

    def rd32(x):
        return next((int.from_bytes(d[x - b:x - b + 4], "little") for b, d in segs
                     if b <= x < b + len(d) - 3), None)

    def unveneer(t):
        """A linker veneer (at most 16 bytes to the next start) holds its target as a word."""
        if nxt.get(t, t + 0x10000) - t > 16:
            return t
        for k in range(0, 16, 2):
            w = rd32(t + k)
            if w and w & 1 and (0x10000000 <= w < 0x11000000 or 0x20000000 <= w < 0x20042000):
                return w & ~1
        return t

    calls |= {unveneer(t) for t in calls}
    corder = sorted(calls)
    cnxt = {s: corder[i + 1] if i + 1 < len(corder) else s + 0x10000 for i, s in enumerate(corder)}

    ours, where = {}, {}
    for o in a.objs:
        for k, v in our_functions(o).items():
            ours.setdefault(k, v)
            where.setdefault(k, o)

    addr_of = {k: v for k, v in known.items()}
    name_at = {}
    for k, v in known.items():
        name_at.setdefault(v, set()).add(k)
    found_from, conflicts = {}, {}

    kstarts = sorted(set(known.values()))

    def image_calls(f):
        addr = addr_of[f]
        size = ours[f][1]
        cap = addr + size + size // 2 + 64
        if f in known:   # lib/src.tsv starts are complete for named code; BL targets can fall inside
            end = min(min((v for v in kstarts if v > addr), default=cap), cap)
        else:
            end = min(cnxt.get(addr, cap), cap)
        th = callseq.theirs(segs, addr, end - addr, {})
        return [unveneer(int(t[1:], 16)) for t in th]

    # Each step takes the best-aligned function that still names something new.
    while True:
        best = None
        for f in (k for k in ours if k in addr_of and ours[k][0]):
            seq = list(ours[f][0])
            th = image_calls(f)
            pairs, dist = align(seq, th, addr_of, name_at)
            new = [(x, t) for x, t in pairs if x not in addr_of and t not in name_at]
            if len(seq) == len(th):
                for x, t in zip(seq, th):
                    if x in addr_of and addr_of[x] != t and name_at.get(t) and x not in name_at[t]:
                        conflicts.setdefault(x, set()).add(f"{f}:{t:08x}")
            rel = dist / max(len(seq), len(th))
            if new and rel <= MAX_EDITS and (best is None or rel < best[0]):
                best = (rel, f, dist, len(seq), new)
        if best is None:
            break
        rel, f, dist, n, new = best
        for x, t in new:
            if x not in addr_of and t not in name_at:
                addr_of[x], name_at[t] = t, {x}
                found_from[x] = f"{f}{'' if dist == 0 else f' (d{dist}/{n})'}"

    if a.debug:
        for f in sorted(k for k in ours if k in addr_of and ours[k][0]):
            seq, th = list(ours[f][0]), image_calls(f)
            print(f"# {f} 0x{addr_of[f]:08x} d{align(seq, th, addr_of, name_at)[1]} ours {len(seq)} image {len(th)}")
            print("#   ours :", " ".join(seq))
            print("#   image:", " ".join(("|".join(sorted(name_at[t])) if t in name_at else f"?{t:x}") for t in th))
    rows = []
    for k, (seq, size) in ours.items():
        if a.prefix and a.prefix not in where[k]:
            continue
        ad = addr_of.get(k)
        ext = cnxt.get(ad, 0) - ad if ad is not None else 0
        rows.append((ad if ad is not None else 1 << 40, k, size, ext,
                     "known" if k in known else found_from.get(k, "-"),
                     " ".join(sorted(conflicts.get(k, ())))))
    rows.sort()
    for ad, k, size, ext, src, conf in rows:
        ads = "-" if ad == 1 << 40 else f"0x{ad:08x}"
        mark = "" if ad == 1 << 40 or abs(size - ext) <= 2 else "  size!"
        print(f"{ads:12} {ext:5} {size:5}  {k:28} from {src}{mark}{'  CONFLICT ' + conf if conf else ''}")
    if a.out:
        with open(a.out, "w") as f:
            for ad, k, size, ext, src, conf in rows:
                if ad != 1 << 40 and src != "known":
                    f.write(f"0x{ad:08x}\t{k}\t{ext}\t1\tcallgraph:{src}\n")
    print(f"{sum(r[0] != 1 << 40 for r in rows)} of {len(rows)} located", file=sys.stderr)


if __name__ == "__main__":
    main()
