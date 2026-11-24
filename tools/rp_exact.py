#!/usr/bin/env python3
"""Build a byte-identical RP2040 image from the C build's objects, placed at their original
addresses; only what no object matches is taken from the original image (docs/EXACT_BUILD.md).

    python3 tools/rp_exact.py IMAGE [--builddir DIR] [--out DIR]   (run in WSL, after wsl_build.sh)

IMAGE is bootloader or main-ode. The inputs are every object and archive member the C build
linked (from its linker map); nothing is recompiled.
"""
import argparse
import bisect
import os
import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TC = "/mnt/c/Tools/arch-arm-none-eabi-14.2.0"
ENV = dict(os.environ, LD_LIBRARY_PATH=f"{TC}/hostlib")
BASE = {"bootloader": 0x10000000, "main-ode": 0x10080000}

R_ARM_ABS32, R_ARM_REL32, R_ARM_THM_CALL, R_ARM_THM_JUMP24, R_ARM_TARGET1 = 2, 3, 10, 30, 38
R_ARM_THM_JUMP11, R_ARM_THM_JUMP8 = 102, 103
# Short Thumb branches: (opcode mask, immediate bits)
SHORT_BRANCH = {R_ARM_THM_JUMP11: (0xf800, 11), R_ARM_THM_JUMP8: (0xff00, 8)}
ABS_TYPES = (R_ARM_ABS32, R_ARM_TARGET1)
# ld's Thumb-only long-branch stub: push {r0}; ldr r0, [pc, #8]; mov ip, r0; pop {r0}; bx ip; nop
VENEER = bytes.fromhex("01b4024884460 1bc60470 0bf".replace(" ", ""))
STT_FUNC = 2
SHT_PROGBITS, SHF_ALLOC = 1, 2
MIN_SEARCH = 12  # unmasked bytes a section needs before a unique byte search may place it


# --- ELF32 little-endian object reader -------------------------------------------------------

class Obj:
    def __init__(self, path, label=None):
        self.path = path
        self.label = label or pathlib.Path(path).name
        d = self.data = pathlib.Path(path).read_bytes()
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2e)
        self.secs = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<10I", d, shoff + i * shentsize)
            self.secs.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link,
                                  info=info, align=align))
        strs = self.secs[shstrndx]
        for s in self.secs:
            s["name"] = self._str(strs, s["name"])
        self.syms = []
        for s in self.secs:
            if s["type"] == 2:  # SHT_SYMTAB
                st = self.secs[s["link"]]
                for k in range(s["size"] // 16):
                    nm, val, sz, info, other, shndx = struct.unpack_from("<IIIBBH", d, s["off"] + k * 16)
                    self.syms.append(dict(name=self._str(st, nm), value=val, size=sz, type=info & 0xf,
                                          bind=info >> 4, shndx=shndx))
        self.rels = {}  # section index -> [(offset, type, symbol index, file offset of the entry)]
        for s in self.secs:
            if s["type"] == 9:  # SHT_REL
                lst = self.rels.setdefault(s["info"], [])
                for k in range(s["size"] // 8):
                    off, info = struct.unpack_from("<II", d, s["off"] + k * 8)
                    lst.append((off, info & 0xff, info >> 8, s["off"] + k * 8))
        names = [s["name"] for s in self.secs]
        self.dup = {n for n in names if names.count(n) > 1}

    def _str(self, sec, i):
        e = self.data.index(b"\0", sec["off"] + i)
        return self.data[sec["off"] + i:e].decode()

    def bytes(self, idx):
        s = self.secs[idx]
        return self.data[s["off"]:s["off"] + s["size"]]

    def mask(self, idx):
        m = bytearray(self.secs[idx]["size"])
        for off, typ, si, ent in self.rels.get(idx, []):
            n = 2 if typ in SHORT_BRANCH else 4
            m[off:off + n] = b"\1" * n
        return m

    def func_name(self, idx):
        for s in self.syms:
            if s["shndx"] == idx and s["type"] == STT_FUNC and s["value"] & ~1 == 0:
                return s["name"]
        return None


def read_archive(path, out):
    """Extract every member of a GNU ar archive into OUT; returns {member name: path}."""
    d = pathlib.Path(path).read_bytes()
    assert d[:8] == b"!<arch>\n", path
    pos, longnames, members = 8, b"", {}
    out.mkdir(parents=True, exist_ok=True)
    while pos + 60 <= len(d):
        name = d[pos:pos + 16].decode().rstrip()
        size = int(d[pos + 48:pos + 58].decode().strip())
        body = d[pos + 60:pos + 60 + size]
        if name == "//":
            longnames = body
        elif name not in ("/", "/SYM64/"):
            if name.startswith("/"):
                o = int(name[1:])
                name = longnames[o:longnames.index(b"\n", o)].decode()
            name = name.rstrip("/")
            if name not in members:  # duplicates are not distinguishable in the map either
                p = out / name
                p.write_bytes(body)
                members[name] = p
        pos += 60 + size + (size & 1)
    return members


def link_inputs(mapfile, builddir, out):
    """The objects the C build linked, as Obj, from its linker map."""
    text = pathlib.Path(mapfile).read_text()
    objs, archives = [], {}
    for m in re.finditer(r"^LOAD (\S+)$", text, re.M):
        p = m.group(1)
        if p.endswith((".o", ".obj")):
            objs.append(Obj(str((builddir / p).resolve()) if not p.startswith("/") else p))
    sect = text.split("Archive member included", 1)[1].split("\n\n", 2)[1]
    seen = set()
    for m in re.finditer(r"^(\S+\.a)\(([^)]+)\)$", sect, re.M):
        a, member = m.group(1), m.group(2)
        if (a, member) in seen:
            continue
        seen.add((a, member))
        ap = (builddir / a).resolve() if not a.startswith("/") else pathlib.Path(a).resolve()
        if ap not in archives:
            archives[ap] = read_archive(ap, out / "ar" / ap.name)
        objs.append(Obj(str(archives[ap][member]), f"{ap.name}({member})"))
    return objs


# --- original image ---------------------------------------------------------------------------

class Image:
    """The original image: flash at BASE, plus RAM regions loaded from flash by crt0."""

    def __init__(self, path, base):
        self.data = pathlib.Path(path).read_bytes()
        self.base = base
        img = self.data
        bi = img.find(struct.pack("<I", 0x7188ebf2))
        table = struct.unpack_from("<I", img, bi + 12)[0] - base
        self.ram = []  # (vma, lma, length)
        while True:
            src, dst, end = struct.unpack_from("<III", img, table)
            if src == 0:
                break
            if end > dst:
                self.ram.append((dst, src, end - dst))
            table += 12

    def offset(self, vma):
        for v, l, n in self.ram:
            if v <= vma < v + n:
                return l - self.base + vma - v
        if self.base <= vma < self.base + len(self.data):
            return vma - self.base
        return None

    def vma(self, off):
        """The run address of file offset OFF."""
        for v, l, n in self.ram:
            if l - self.base <= off < l - self.base + n:
                return v + off - (l - self.base)
        return self.base + off

    def read(self, vma, n):
        o = self.offset(vma)
        return None if o is None else self.data[o:o + n]


def bl_target(h1, h2, pc):
    s = (h1 >> 10) & 1
    imm = ((-s) << 24) | ((~((h2 >> 13) & 1) ^ s) & 1) << 23 | ((~((h2 >> 11) & 1) ^ s) & 1) << 22 \
        | (h1 & 0x3ff) << 12 | (h2 & 0x7ff) << 1
    return pc + 4 + imm


# --- matching -----------------------------------------------------------------------------------

def placeable(o, idx):
    s = o.secs[idx]
    return (s["type"] == SHT_PROGBITS and s["flags"] & SHF_ALLOC and s["size"] > 0
            and s["flags"] & 0x30 != 0x30 and s["name"] not in o.dup)


def constraints(img, addr, o, idx):
    """Check a section's bytes against the original at ADDR and derive what each relocation must
    resolve to.

    Returns (None, reason) on mismatch, else (list of (key, value, is_call), None). A key is
    ('sec', obj path, section index) for a symbol defined in an object, ('abs', name), or
    ('fix', obj path, rel entry file offset) for a word the linker produced itself and whose
    original value the copy takes: a pointer into a merged string section (laid out per string;
    the string there is checked) or a BL through a long-branch veneer (its target is checked).
    """
    code = o.bytes(idx)
    orig = img.read(addr, len(code))
    if orig is None or len(orig) != len(code):
        return None, "outside the image"
    mask = o.mask(idx)
    for i in range(len(code)):
        if not mask[i] and code[i] != orig[i]:
            return None, f"differs at +0x{i:x}"
    out = []
    for off, typ, si, ent in o.rels.get(idx, []):
        sym = o.syms[si]
        call = False
        if sym["bind"] == 2 and sym["shndx"] == 0:
            # Undefined weak: the original linked no definition either if its word is the bare
            # addend (address 0) or its BL is ld's rewrite for such a call, b.n +4; nop.
            # Otherwise it is resolved like any other symbol.
            if typ in ABS_TYPES:
                none = orig[off:off + 4] == code[off:off + 4]
            else:
                none = typ in (R_ARM_THM_CALL, R_ARM_THM_JUMP24) and \
                    struct.unpack_from("<HH", orig, off) == (0xe000, 0xbf00)
            if none:
                out.append((("weak", sym["name"]), 0, False))
                continue
        if typ in ABS_TYPES or typ == R_ARM_REL32:
            a, = struct.unpack_from("<I", code, off)
            w, = struct.unpack_from("<I", orig, off)
            val = (w - a + (addr + off if typ == R_ARM_REL32 else 0)) & 0xffffffff
        elif typ in SHORT_BRANCH:
            opm, bits = SHORT_BRANCH[typ]
            h, = struct.unpack_from("<H", orig, off)
            c, = struct.unpack_from("<H", code, off)
            if h & opm != c & opm:
                return None, f"differs at +0x{off:x}"
            imm = lambda x: ((x & ((1 << bits) - 1)) ^ (1 << (bits - 1))) - (1 << (bits - 1))
            a = imm(c) * 2   # the REL addend, normally -4
            val, call = addr + off + 4 + imm(h) * 2 - 4 - a, True
        elif typ in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
            h1, h2 = struct.unpack_from("<HH", orig, off)
            if h1 & 0xf800 != 0xf000:
                return None, f"no BL at +0x{off:x}"
            c1, c2 = struct.unpack_from("<HH", code, off)
            a = bl_target(c1, c2, 0) - 4   # the REL addend, normally -4
            t = bl_target(h1, h2, addr + off)
            stub = img.read(t, len(VENEER) + 4)
            if stub and stub[:len(VENEER)] == VENEER:
                out.append((("fix", o.path, ent), struct.unpack_from("<I", orig, off)[0], False))
                t = struct.unpack_from("<I", stub, len(VENEER))[0] & ~1
            val, call = t - 4 - a, True
        else:
            return None, f"relocation type {typ} at +0x{off:x}"
        tsec = o.secs[sym["shndx"]] if 0 < sym["shndx"] < 0xff00 else None
        if tsec and typ in ABS_TYPES and tsec["flags"] & 0x30 == 0x30:  # SHF_MERGE | SHF_STRINGS
            ours = o.bytes(sym["shndx"])
            a += sym["value"]
            want = ours[a:ours.index(b"\0", a) + 1]
            if img.read(w, len(want)) != want:
                return None, f"string at +0x{off:x} differs"
            out.append((("fix", o.path, ent), w, False))
        elif tsec and sym["bind"] == 0:  # local: the section here
            v = sym["value"] & ~1 if call else sym["value"]
            out.append((("sec", o.path, sym["shndx"]), (val - v) & 0xffffffff, call))
        elif sym["shndx"] == 0xfff1:  # SHN_ABS: nothing to place
            continue
        else:
            out.append((("abs", sym["name"]), val | (1 if call else 0), call))
    return out, None


class Placer:
    def __init__(self, img, objs):
        self.img, self.objs = img, objs
        self.by_path = {o.path: o for o in objs}
        self.defs = {}  # global name -> (path, idx, value); strong definitions win over weak
        for o in objs:
            for sym in o.syms:
                if sym["bind"] in (1, 2) and 0 < sym["shndx"] < 0xff00 and sym["name"]:
                    if sym["name"] not in self.defs or sym["bind"] == 1:
                        self.defs[sym["name"]] = (o.path, sym["shndx"], sym["value"])
        self.known = {}      # key -> value (section addresses, absolute symbols, fixed words)
        self.setby = {}
        self.placed = {}     # (path, idx) -> addr
        self.tried = {}      # (path, idx) -> reason it was not placed, or None
        self.occupied = []   # sorted (start, end) of placed ranges

    def name(self, key):
        if key[0] == "sec":
            o = self.by_path[key[1]]
            return o.func_name(key[2]) or f"{o.label}:{o.secs[key[2]]['name']}"
        return key[1]

    def seed(self, key, addr, why):
        self.known[key] = addr
        self.setby[key] = why

    def free(self, a, n):
        i = bisect.bisect_right(self.occupied, (a, 1 << 40))
        if i and self.occupied[i - 1][1] > a:
            return False
        return i == len(self.occupied) or self.occupied[i][0] >= a + n

    def attempt(self, key):
        """Place section KEY at its known address if its bytes and relocations agree."""
        _, path, idx = key
        o = self.by_path[path]
        addr = self.known[key]
        size = o.secs[idx]["size"]
        if not self.free(addr, size):
            return "overlaps a placed section"
        cons, why = constraints(self.img, addr, o, idx)
        if cons is None:
            return why
        trial = {}
        for k, val, call in cons:
            if k[0] == "fix":
                trial[k] = val
                continue
            if k[0] == "weak":
                if k[1] in self.defs:
                    return f"{k[1]} is defined here but not in the original"
                trial[k] = val
                continue
            if k[0] == "abs" and k[1] in self.defs:  # defined in one of our objects
                p, i, v = self.defs[k[1]]
                k = ("sec", p, i)
                val = ((val & ~1) - (v & ~1) if call else val - v) & 0xffffffff
            have = trial.get(k, self.known.get(k))
            if have is not None and have != val:
                return (f"{self.name(k)} would be at 0x{val:x}, not 0x{have:x}"
                        f" (from {self.setby.get(k, 'an earlier relocation here')})")
            trial[k] = val
        for k, v in trial.items():
            if k not in self.known:
                self.known[k] = v
                self.setby[k] = self.name(key)
        self.placed[(path, idx)] = addr
        bisect.insort(self.occupied, (addr, addr + size))
        return None

    def search(self, o, idx):
        """The unique address where the section's unmasked bytes occur, or None."""
        code, mask = o.bytes(idx), o.mask(idx)
        if len(code) - sum(mask) < MIN_SEARCH:
            return None
        best, run, start = (0, 0), 0, 0
        for i, m in enumerate(mask + b"\1"):
            if m:
                if i - start > best[1] - best[0]:
                    best = (start, i)
                start = i + 1
        anchor = code[best[0]:best[1]]
        data, hits, p = self.img.data, [], -1
        align = max(o.secs[idx]["align"], 1)
        while len(hits) < 2:
            p = data.find(anchor, p + 1)
            if p < 0:
                break
            s = p - best[0]
            if s < 0 or s + len(code) > len(data):
                continue
            if all(mask[i] or code[i] == data[s + i] for i in range(len(code))):
                v = self.img.vma(s)
                if v % align == 0 and self.img.vma(s + len(code) - 1) == v + len(code) - 1:
                    hits.append(v)
        return hits[0] if len(hits) == 1 else None

    def run(self):
        while True:
            progress = False
            # Sections whose address is known: try them in address order.
            todo = sorted((v, k) for k, v in self.known.items()
                          if k[0] == "sec" and (k[1], k[2]) not in self.tried
                          and placeable(self.by_path[k[1]], k[2]))
            for _, k in todo:
                self.tried[(k[1], k[2])] = self.attempt(k)
                progress = True
            if progress:
                continue
            # Nothing pending: look for the rest by unique byte search.
            for o in self.objs:
                for idx in range(len(o.secs)):
                    k = ("sec", o.path, idx)
                    if k in self.known or not placeable(o, idx):
                        continue
                    a = self.search(o, idx)
                    if a is not None and self.free(a, o.secs[idx]["size"]):
                        self.seed(k, a, "byte search")
                        progress = True
            if not progress:
                return


# --- output ---------------------------------------------------------------------------------------

def merged_strings(objs):
    """Every NUL-terminated string in the objects' merged string sections."""
    out = set()
    for o in objs:
        for i, s in enumerate(o.secs):
            if s["flags"] & 0x30 == 0x30 and s["flags"] & SHF_ALLOC:
                for part in o.bytes(i).split(b"\0"):
                    if part:
                        out.add(part + b"\0")
    return out


def classify(img, vma, data, strings, known_funcs):
    """Split a gap into ('zero', n), ('veneer', bytes), ('string', bytes) and ('orig', n) pieces."""
    pieces, i, n = [], 0, len(data)

    def add(kind, b):
        if pieces and pieces[-1][0] == kind:
            pieces[-1] = (kind, pieces[-1][1] + b)
        else:
            pieces.append((kind, b))

    while i < n:
        if data[i] == 0:
            add("zero", b"\0")
            i += 1
            continue
        if data[i:i + len(VENEER)] == VENEER and i + len(VENEER) + 4 <= n:
            t, = struct.unpack_from("<I", data, i + len(VENEER))
            if t & ~1 in known_funcs:
                add("veneer", data[i:i + len(VENEER) + 4])
                i += len(VENEER) + 4
                continue
        e = data.find(b"\0", i)
        if e >= 0 and e - i >= 2 and data[i:e + 1] in strings:
            add("string", data[i:e + 1])
            i = e + 1
            continue
        add("orig", data[i:i + 1])
        i += 1
    return pieces


def owners(img_name, pl):
    """(start, end, name) for attributing original bytes: syms/<image>.lib.tsv functions with
    their sizes, unplaced sections whose address is known with our size, and src.tsv functions
    up to the next of all these starts."""
    out, starts = [], set()
    p = ROOT / "syms" / f"{img_name}.lib.tsv"
    for line in p.read_text().splitlines() if p.exists() else []:
        f = line.split("	")
        a = int(f[0], 16)
        out.append((a, a + int(f[2]), f[1]))
    for k, v in pl.known.items():
        if k[0] == "sec" and (k[1], k[2]) not in pl.placed:
            o = pl.by_path[k[1]]
            if o.secs[k[2]]["flags"] & SHF_ALLOC and o.secs[k[2]]["type"] == SHT_PROGBITS:
                out.append((v, v + o.secs[k[2]]["size"], pl.name(k)))
    starts = sorted({a for a, _, _ in out} | set(pl.placed.values()))
    for line in (ROOT / "syms" / f"{img_name}.src.tsv").read_text().splitlines():
        f = line.split("	")
        a = int(f[0], 16)
        nxt = [s for s in starts if s > a]
        out.append((a, nxt[0] if nxt else a + 0x10000, f[1]))
    return sorted(out)


def write_link(img, img_name, pl, out, copy, strings, report):
    """Write gaps.s (original bytes and verified linker data), syms.s (absolute symbols) and
    exact.ld. COPY maps each object's path to the relocation-stripped copy that is linked."""
    spans = []
    holes = sorted((l - img.base, l - img.base + n, v) for v, l, n in img.ram)
    pos = 0
    for a, b, v in holes:
        if a > pos:
            spans.append((img.base + pos, img.base + pos, a - pos))
        spans.append((v, img.base + a, b - a))
        pos = b
    if pos < len(img.data):
        spans.append((img.base + pos, img.base + pos, len(img.data) - pos))

    known_funcs = {v & ~1 for v in pl.known.values()} | {a for a in pl.placed.values()}
    gaps, ld = [], []
    bin_name = f"flippydrive-{img_name}.bin"
    ld.append("SECTIONS\n{")
    placed = sorted((a, p, i) for (p, i), a in pl.placed.items())
    for n, (vma, lma, length) in enumerate(spans):
        inside = [(a, p, i) for a, p, i in placed if vma <= a < vma + length]
        ld.append(f"    .span{n} 0x{vma:08x} : AT(0x{lma:08x})\n    {{")
        cur = vma
        for addr, path, idx in inside + [(vma + length, None, None)]:
            if addr > cur:
                g = len(gaps)
                body = []
                off = img.offset(cur)
                p = 0
                for kind, b in classify(img, cur, img.data[off:off + addr - cur], strings, known_funcs):
                    report.append((cur + p, len(b), kind))
                    if kind == "orig":
                        body.append(f'    .incbin "{bin_name}", 0x{off + p:x}, 0x{len(b):x}\n')
                    elif kind == "zero":
                        body.append(f"    .space 0x{len(b):x}\n")
                    else:
                        body.append("    .byte " + ",".join(str(x) for x in b) + "\n")
                    p += len(b)
                gaps.append(f'    .section .gap{g}, "a"\n    .p2align 0\n' + "".join(body))
                ld.append(f"        *(.gap{g})")
            if path is None:
                break
            o = pl.by_path[path]
            nm = pl.name(("sec", path, idx))
            ld.append(f'        ASSERT(ABSOLUTE(.) == 0x{addr:08x}, "{nm} is not at 0x{addr:08x}");')
            ld.append(f'        "{copy[path]}"({o.secs[idx]["name"]})')
            report.append((addr, o.secs[idx]["size"], "C"))
            cur = addr + o.secs[idx]["size"]
        ld.append(f'        ASSERT(ABSOLUTE(.) == 0x{vma + length:08x}, "span {n} ends at the wrong address");')
        ld.append("    }")
    # Sections the placed ones refer to but which are not placed: addressed, not loaded.
    k = 0
    for key, val in sorted(pl.known.items(), key=lambda kv: str(kv[0])):
        if key[0] == "sec" and (key[1], key[2]) not in pl.placed:
            o = pl.by_path[key[1]]
            if o.secs[key[2]]["name"] in o.dup:
                continue
            ld.append(f'    .ref{k} 0x{val:08x} (NOLOAD) : {{ "{copy[key[1]]}"({o.secs[key[2]]["name"]}) }}')
            k += 1
    ld.append("    /DISCARD/ : { *(*) }\n}")
    syms = []
    for key, val in sorted(pl.known.items(), key=lambda kv: str(kv[0])):
        if key[0] == "abs":
            syms.append(f"    .global {key[1]}\n" + (f"    .type {key[1]}, %function\n" if val & 1 else "")
                        + f"    .set {key[1]}, 0x{val:08x}\n")
    (out / "gaps.s").write_text("    .syntax unified\n" + "".join(gaps))
    (out / "syms.s").write_text("    .syntax unified\n" + "".join(syms))
    (out / "exact.ld").write_text("\n".join(ld) + "\n")


def run(cmd, **kw):
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"{' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", choices=sorted(BASE))
    ap.add_argument("--builddir", help="the C build (default ~/fd-build/fw)")
    ap.add_argument("--out")
    args = ap.parse_args()
    name = args.image
    builddir = pathlib.Path(args.builddir or os.path.expanduser("~/fd-build/fw"))
    out = pathlib.Path(args.out or os.path.expanduser(f"~/fd-build/exact-{name}"))
    out.mkdir(parents=True, exist_ok=True)
    orig_path = ROOT / "original" / "extracted" / f"flippydrive-{name}.bin"
    img = Image(orig_path, BASE[name])
    objs = link_inputs(builddir / f"flippydrive-{name}.elf.map", builddir, out)

    pl = Placer(img, objs)
    # src.tsv addresses are fixed up front, whether or not the function matches, so a call to
    # the wrong function shows up as a conflict instead of moving the callee.
    src = {}
    for line in (ROOT / "syms" / f"{name}.src.tsv").read_text().splitlines():
        a, n = line.split("\t")[:2]
        src.setdefault(n, int(a, 16))
    cands = []
    app_dir = str((builddir / name / "CMakeFiles" / f"flippydrive-{name}.dir").resolve())
    for o in objs:
        if not o.path.startswith(app_dir):
            continue
        for sym in o.syms:
            if sym["type"] == STT_FUNC and sym["name"] in src and 0 < sym["shndx"] < 0xff00 \
                    and sym["value"] & ~1 == 0 and sym["size"] == o.secs[sym["shndx"]]["size"]:
                pl.seed(("sec", o.path, sym["shndx"]), src[sym["name"]], "src.tsv")
                cands.append((sym["name"], src[sym["name"]], o.path, sym["shndx"]))
    pl.run()

    # The linked copies: unplaced sections only lend an address (NOLOAD), so their relocations
    # are dropped; words the linker made take the original's value.
    (out / "obj").mkdir(exist_ok=True)
    copy = {}
    for k, o in enumerate(objs):
        copy[o.path] = str(out / "obj" / f"{k:03d}_{pathlib.Path(o.path).name}")
        data = bytearray(o.data)
        shoff, = struct.unpack_from("<I", data, 0x20)
        for i, sec in enumerate(o.secs):
            if sec["flags"] & SHF_ALLOC and (o.path, i) not in pl.placed:
                struct.pack_into("<I", data, shoff + i * 40 + 32, 1)  # sh_addralign
        for i, rels in o.rels.items():
            if (o.path, i) not in pl.placed:
                continue
            for off, typ, si, ent in rels:
                if ("fix", o.path, ent) in pl.known:
                    struct.pack_into("<I", data, o.secs[i]["off"] + off, pl.known[("fix", o.path, ent)])
                    struct.pack_into("<I", data, ent + 4, 0)  # R_ARM_NONE, no symbol
        tmp = pathlib.Path(copy[o.path] + ".tmp")
        tmp.write_bytes(data)
        strip = [f"--remove-relocations={o.secs[i]['name']}" for i in o.rels if (o.path, i) not in pl.placed]
        run([f"{TC}/bin/arm-none-eabi-objcopy"] + strip + [str(tmp), copy[o.path]])
        tmp.unlink()
    report = []
    write_link(img, name, pl, out, copy, merged_strings(objs), report)

    gcc = f"{TC}/bin/arm-none-eabi-gcc"
    for s in ("gaps", "syms"):
        run([gcc, "-mcpu=cortex-m0plus", "-mthumb", "-c", f"-Wa,-I{orig_path.parent}", f"{s}.s", "-o", f"{s}.o"],
            cwd=out)
    run([gcc, "-nostdlib", "-nostartfiles", "-Wl,-T,exact.ld", "-Wl,--no-check-sections", "-Wl,--no-warn-rwx-segments",
         "-Wl,-Map,exact.map", "-o", "exact.elf", "gaps.o", "syms.o"] + list(copy.values()), cwd=out)
    run([f"{TC}/bin/arm-none-eabi-objcopy", "-O", "binary", "exact.elf", f"flippydrive-{name}.bin"], cwd=out)

    # status.tsv: the src.tsv functions; sections.tsv: every placed section; gaps.tsv: what is
    # still the original's bytes, by owning function where syms/ knows it.
    with open(out / "status.tsv", "w") as f:
        for n, a, path, idx in sorted(cands, key=lambda c: c[1]):
            why = pl.tried.get((path, idx))
            f.write(f"0x{a:08x}\t{n}\t" + ("C\t" + pathlib.Path(path).name[:-2] if (path, idx) in pl.placed
                                           else f"original\t{why}") + "\n")
    with open(out / "sections.tsv", "w") as f:
        for (path, idx), a in sorted(pl.placed.items(), key=lambda kv: kv[1]):
            o = pl.by_path[path]
            f.write(f"0x{a:08x}\t{o.secs[idx]['size']}\t{o.label}\t{o.secs[idx]['name']}\t"
                    f"{pl.setby.get(('sec', path, idx), '')}\n")
    with open(out / "unplaced.tsv", "w") as f:
        for o in objs:
            for idx in range(len(o.secs)):
                if not placeable(o, idx) or (o.path, idx) in pl.placed:
                    continue
                a = pl.known.get(("sec", o.path, idx))
                why = pl.tried.get((o.path, idx)) or ("not tried" if a is not None else "no address")
                f.write(f"{'' if a is None else f'0x{a:08x}'}\t{o.secs[idx]['size']}\t{o.label}\t"
                        f"{o.secs[idx]['name']}\t{why}\n")
    own = owners(name, pl)
    totals = {}
    with open(out / "gaps.tsv", "w") as f:
        for a, n, kind in sorted(report):
            totals[kind] = totals.get(kind, 0) + n
            if kind == "orig":
                who = next((w for s, e, w in reversed(own) if s <= a < e), "")
                f.write(f"0x{a:08x}\t{n}\t{who}\n")
    placed_src = sum(1 for _, _, p, i in cands if (p, i) in pl.placed)
    print(f"{name}: {placed_src} of {len(cands)} src.tsv functions from C; {len(pl.placed)} sections placed")
    print(f"  bytes: C {totals.get('C', 0)}, zero fill {totals.get('zero', 0)}, strings {totals.get('string', 0)}, "
          f"veneers {totals.get('veneer', 0)}, original {totals.get('orig', 0)}  (gaps.tsv)")

    built = (out / f"flippydrive-{name}.bin").read_bytes()
    if built == img.data:
        print(f"IDENTICAL: {out}/flippydrive-{name}.bin")
    else:
        diff = next((i for i in range(min(len(built), len(img.data))) if built[i] != img.data[i]),
                    min(len(built), len(img.data)))
        sys.exit(f"DIFFERS: built 0x{len(built):x} bytes, original 0x{len(img.data):x}; "
                 f"first difference at file offset 0x{diff:x}")


if __name__ == "__main__":
    main()
