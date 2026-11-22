#!/usr/bin/env python3
"""Build a byte-identical RP2040 image: exact C functions linked at their original addresses,
everything else taken from the original image (docs/EXACT_BUILD.md).

    python3 tools/rp_exact.py IMAGE [--objdir DIR] [--out DIR]     (run in WSL, after wsl_build.sh)

IMAGE is bootloader or main-ode. The C build's objects are the input; nothing is recompiled.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TC = "/mnt/c/Tools/arch-arm-none-eabi-14.2.0"
ENV = dict(os.environ, LD_LIBRARY_PATH=f"{TC}/hostlib")
BASE = {"bootloader": 0x10000000, "main-ode": 0x10080000}

R_ARM_ABS32, R_ARM_THM_CALL, R_ARM_THM_JUMP24 = 2, 10, 30
# ld's Thumb-only long-branch stub: push {r0}; ldr r0, [pc, #8]; mov ip, r0; pop {r0}; bx ip; nop
VENEER = bytes.fromhex("01b4024884460 1bc60470 0bf".replace(" ", ""))
STT_FUNC, STT_SECTION = 2, 3


# --- ELF32 little-endian object reader -------------------------------------------------------

class Obj:
    def __init__(self, path):
        self.path = path
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

    def _str(self, sec, i):
        e = self.data.index(b"\0", sec["off"] + i)
        return self.data[sec["off"] + i:e].decode()

    def bytes(self, idx):
        s = self.secs[idx]
        return self.data[s["off"]:s["off"] + s["size"]]


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

    def lma(self, vma):
        o = self.offset(vma)
        return None if o is None else self.base + o

    def read(self, vma, n):
        o = self.offset(vma)
        return None if o is None else self.data[o:o + n]


def bl_target(h1, h2, pc):
    s = (h1 >> 10) & 1
    imm = ((-s) << 24) | ((~((h2 >> 13) & 1) ^ s) & 1) << 23 | ((~((h2 >> 11) & 1) ^ s) & 1) << 22 \
        | (h1 & 0x3ff) << 12 | (h2 & 0x7ff) << 1
    return pc + 4 + imm


# --- matching -----------------------------------------------------------------------------------

def candidates(img_name, objs):
    """(name, addr, obj, section index) for every src.tsv function defined in OBJS."""
    want = {}
    for line in (ROOT / "syms" / f"{img_name}.src.tsv").read_text().splitlines():
        a, n = line.split("\t")
        want.setdefault(n, int(a, 16))
    out = []
    for o in objs:
        for sym in o.syms:
            if sym["type"] == STT_FUNC and sym["name"] in want and 0 < sym["shndx"] < 0xff00 \
                    and sym["value"] & ~1 == 0 and sym["size"] == o.secs[sym["shndx"]]["size"]:
                out.append((sym["name"], want[sym["name"]], o, sym["shndx"]))
    return sorted(out, key=lambda c: c[1])


def constraints(img, addr, o, idx):
    """Check FUNC's bytes against the original and derive what each relocation must resolve to.

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
    mask = bytearray(len(code))
    out = []
    for off, typ, si, ent in o.rels.get(idx, []):
        sym = o.syms[si]
        if typ == R_ARM_ABS32:
            a, = struct.unpack_from("<I", code, off)
            w, = struct.unpack_from("<I", orig, off)
            val, call = (w - a) & 0xffffffff, False
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
            return None, f"relocation type {typ}"
        mask[off:off + 4] = b"\1\1\1\1"
        tsec = o.secs[sym["shndx"]] if 0 < sym["shndx"] < 0xff00 else None
        if tsec and typ == R_ARM_ABS32 and tsec["flags"] & 0x30 == 0x30:  # SHF_MERGE | SHF_STRINGS
            ours = o.bytes(sym["shndx"])
            a += sym["value"]
            want = ours[a:ours.index(b"\0", a) + 1]
            if img.read(w, len(want)) != want:
                return None, f"string at +0x{off:x} differs"
            out.append((("fix", o.path, ent), w, False))
        elif tsec:
            v = sym["value"] & ~1 if call else sym["value"]
            out.append((("sec", o.path, sym["shndx"]), (val - v) & 0xffffffff, call))
        else:
            out.append((("abs", sym["name"]), val | (1 if call else 0), call))
    for i in range(len(code)):
        if not mask[i] and code[i] != orig[i]:
            return None, f"differs at +0x{i:x}"
    return out, None


def resolve(img, cands, objs):
    """Pick the exact functions whose relocations agree, and the address of everything they use."""
    defs = {}  # global name -> ('sec', path, idx, value)
    for o in objs:
        for sym in o.syms:
            if sym["bind"] in (1, 2) and 0 < sym["shndx"] < 0xff00 and sym["name"]:
                defs[sym["name"]] = (o.path, sym["shndx"], sym["value"])
    placed, dropped = [], []
    # src.tsv addresses are known whether or not the function is exact, so a call to the wrong
    # function shows up as a conflict instead of moving the callee.
    known = {("sec", o.path, idx): addr for _, addr, o, idx in cands}
    names = {("sec", o.path, idx): n for n, _, o, idx in cands}
    setby = {k: "src.tsv" for k in known}
    for name, addr, o, idx in cands:
        cons, why = constraints(img, addr, o, idx)
        if cons is None:
            dropped.append((name, addr, why))
            continue
        trial = dict(known)
        bad = None
        for key, val, call in cons:
            if key[0] == "fix":
                trial[key] = val
                continue
            label = key[1] if key[0] == "abs" else o.secs[key[2]]["name"]
            if key[0] == "abs" and key[1] in defs:  # defined in another of our objects
                p, i, v = defs[key[1]]
                key = ("sec", p, i)
                val = ((val & ~1) - (v & ~1) if call else val - v) & 0xffffffff
            if key in trial and trial[key] != val:
                bad = (f"{names.get(key, label)} would be at 0x{val:x}, not 0x{trial[key]:x}"
                       f" (from {setby.get(key, 'an earlier relocation here')})")
                break
            trial[key] = val
        if bad:
            dropped.append((name, addr, bad))
            continue
        for k in trial:
            setby.setdefault(k, name)
        known = trial
        placed.append((name, addr, o, idx))
    return placed, dropped, known


# --- output ---------------------------------------------------------------------------------------

def write_link(img, img_name, placed, known, objs, out, copy):
    """Write gaps.s (original bytes), syms.s (absolute symbols) and exact.ld. COPY maps each
    object's path to the relocation-stripped copy that is linked."""
    by_path = {o.path: o for o in objs}
    loaded = {("sec", o.path, idx): addr for _, addr, o, idx in placed}
    # Address ranges of the image, in file order: (vma, lma, length).
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

    gaps, ld = [], []
    bin_name = f"flippydrive-{img_name}.bin"
    ld.append("SECTIONS\n{")
    for n, (vma, lma, length) in enumerate(spans):
        inside = sorted((addr, name, o, idx) for name, addr, o, idx in placed if vma <= addr < vma + length)
        ld.append(f"    .span{n} 0x{vma:08x} : AT(0x{lma:08x})\n    {{")
        cur = vma
        for addr, name, o, idx in inside + [(vma + length, None, None, None)]:
            if addr > cur:
                g = len(gaps)
                gaps.append(f'    .section .gap{g}, "a"\n    .p2align 0\n'
                            f'    .incbin "{bin_name}", 0x{img.offset(cur):x}, 0x{addr - cur:x}\n')
                ld.append(f"        *(.gap{g})")
            if name is None:
                break
            ld.append(f'        ASSERT(ABSOLUTE(.) == 0x{addr:08x}, "{name} is not at 0x{addr:08x}");')
            ld.append(f'        "{copy[o.path]}"({o.secs[idx]["name"]})')
            cur = addr + o.secs[idx]["size"]
        ld.append(f'        ASSERT(ABSOLUTE(.) == 0x{vma + length:08x}, "span {n} ends at the wrong address");')
        ld.append("    }")
    # Sections the exact functions refer to but which are not exact themselves: addressed, not loaded.
    k = 0
    for key, val in sorted(known.items(), key=lambda kv: str(kv[0])):
        if key[0] == "sec" and key not in loaded:
            o = by_path[key[1]]
            ld.append(f'    .ref{k} 0x{val:08x} (NOLOAD) : {{ "{copy[o.path]}"({o.secs[key[2]]["name"]}) }}')
            k += 1
    ld.append("    /DISCARD/ : { *(*) }\n}")
    syms = []
    for key, val in sorted(known.items(), key=lambda kv: str(kv[0])):
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
    ap.add_argument("--objdir")
    ap.add_argument("--out")
    args = ap.parse_args()
    name = args.image
    objdir = pathlib.Path(args.objdir or os.path.expanduser(
        f"~/fd-build/fw/{name}/CMakeFiles/flippydrive-{name}.dir"))
    out = pathlib.Path(args.out or os.path.expanduser(f"~/fd-build/exact-{name}"))
    out.mkdir(parents=True, exist_ok=True)
    orig_path = ROOT / "original" / "extracted" / f"flippydrive-{name}.bin"
    img = Image(orig_path, BASE[name])
    objs = [Obj(str(objdir / f"{c.name}.o")) for c in sorted((ROOT / name).glob("*.c"))
            if (objdir / f"{c.name}.o").exists()]

    cands = candidates(name, objs)
    placed, dropped, known = resolve(img, cands, objs)
    # Sections that are not placed only lend an address (NOLOAD); their relocations would pull in
    # everything they call, so the linked copies drop them.
    (out / "obj").mkdir(exist_ok=True)
    loaded = {(o.path, idx) for _, _, o, idx in placed}
    copy = {}
    for o in objs:
        copy[o.path] = str(out / "obj" / pathlib.Path(o.path).name)
        data = bytearray(o.data)
        shoff, = struct.unpack_from("<I", data, 0x20)
        for i, sec in enumerate(o.secs):
            if sec["flags"] & 2 and (o.path, i) not in loaded:  # SHF_ALLOC: an address only
                struct.pack_into("<I", data, shoff + i * 40 + 32, 1)  # sh_addralign
        for i, rels in o.rels.items():
            if (o.path, i) not in loaded:
                continue
            for off, typ, si, ent in rels:
                if ("fix", o.path, ent) in known:
                    struct.pack_into("<I", data, o.secs[i]["off"] + off, known[("fix", o.path, ent)])
                    struct.pack_into("<I", data, ent + 4, 0)  # R_ARM_NONE, no symbol
        tmp = out / "obj" / (pathlib.Path(o.path).name + ".tmp")
        tmp.write_bytes(data)
        strip = [f"--remove-relocations={o.secs[i]['name']}" for i in o.rels if (o.path, i) not in loaded]
        run([f"{TC}/bin/arm-none-eabi-objcopy"] + strip + [str(tmp), copy[o.path]])
        tmp.unlink()
    write_link(img, name, placed, known, objs, out, copy)

    gcc = f"{TC}/bin/arm-none-eabi-gcc"
    for s in ("gaps", "syms"):
        run([gcc, "-mcpu=cortex-m0plus", "-mthumb", "-c", f"-Wa,-I{orig_path.parent}", f"{s}.s", "-o", f"{s}.o"],
            cwd=out)
    run([gcc, "-nostdlib", "-nostartfiles", "-Wl,-T,exact.ld", "-Wl,--no-check-sections", "-Wl,--no-warn-rwx-segments",
         "-Wl,-Map,exact.map", "-o", "exact.elf", "gaps.o", "syms.o"] + list(copy.values()), cwd=out)
    run([f"{TC}/bin/arm-none-eabi-objcopy", "-O", "binary", "exact.elf", f"flippydrive-{name}.bin"], cwd=out)

    built = (out / f"flippydrive-{name}.bin").read_bytes()
    with open(out / "status.tsv", "w") as f:
        for n, a, o, idx in placed:
            f.write(f"0x{a:08x}\t{n}\tC\t{pathlib.Path(o.path).name[:-2]}\n")
        for n, a, why in dropped:
            f.write(f"0x{a:08x}\t{n}\toriginal\t{why}\n")
    c_bytes = sum(o.secs[idx]["size"] for _, _, o, idx in placed)
    print(f"{name}: {len(placed)} of {len(cands)} functions from C ({c_bytes} bytes), "
          f"{len(dropped)} from the original; status in {out}/status.tsv")
    if built == img.data:
        print(f"IDENTICAL: {out}/flippydrive-{name}.bin")
    else:
        diff = next((i for i in range(min(len(built), len(img.data))) if built[i] != img.data[i]),
                    min(len(built), len(img.data)))
        sys.exit(f"DIFFERS: built 0x{len(built):x} bytes, original 0x{len(img.data):x}; "
                 f"first difference at file offset 0x{diff:x}")


if __name__ == "__main__":
    main()
