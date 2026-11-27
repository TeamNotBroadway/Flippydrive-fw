#!/usr/bin/env python3
"""Set up a decomp-permuter directory for one application function (run under WSL).

    python3 tools/dp_setup.py IMAGE FUNC [OUTDIR]       (default permute_out/dp/FUNC)
    then: PYTHONPATH=~/ext/dp-shim python3 ~/ext/decomp-permuter/permuter.py OUTDIR -j8

decomp-permuter (github.com/simonlindholm/decomp-permuter, cloned to ~/ext; ~/ext/dp-shim holds
a tomllib stand-in for its `toml` import) compares objdump output of target.o with each
candidate. Here both sides are linked at the function's original address:

  target.o   the original's bytes for FUNC (to the next known function start), with $t/$d
             mapping symbols so literal pools disassemble as data
  base.c     FUNC's source file preprocessed with the build's flags; bodies of other
             non-inline functions are dropped, static inline helpers are kept
  compile.sh compiles with the build's flags, then links FUNC's section at its address with
             every symbol of the exact build defined at its address (PROVIDE), so calls and
             loads of globals disassemble alike

String literals and variables defined in base.c itself land elsewhere, so their pool words
can differ on every candidate; confirm a result with tools/fnscore.py.
"""
import os, pathlib, re, subprocess, sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import permute as P
import equiv as E
import callseq
from symmatch import segments

TC = os.path.dirname(os.path.dirname(E.OBJDUMP))


def strip_bodies(src, keep, keep_inline=True):
    """Drop bodies of functions other than KEEP, except inline ones if KEEP_INLINE."""
    out, i = [], 0
    fn = re.compile(r"^[^\n#;{}]*?\b(\w+)\s*\([^;{}]*\)\s*(?:__attribute__\s*\(\(.*?\)\)\s*)*\{", re.M | re.S)
    while True:
        m = fn.search(src, i)
        if not m:
            out.append(src[i:])
            break
        depth, j = 0, m.end() - 1
        while True:
            depth += {"{": 1, "}": -1}.get(src[j], 0)
            j += 1
            if depth == 0:
                break
        head = src[m.start():m.end() - 1]
        out.append(src[i:m.start()])
        if m.group(1) == keep or (keep_inline and "inline" in head):
            out.append(src[m.start():j])
        else:
            out.append(head.rstrip() + ";")
        i = j
    return "".join(out)


def main():
    img, func = sys.argv[1:3]
    root = pathlib.Path(E.ROOT)
    out = pathlib.Path(sys.argv[3] if len(sys.argv) > 3 else root / "permute_out/dp" / func).resolve()
    out.mkdir(parents=True, exist_ok=True)
    src = E.source_of(img, func)
    if src is None:
        sys.exit(f"no source in {img}/ defines {func}")
    flags = P.build_flags(img, str(src))

    elf = pathlib.Path.home() / f"fd-build/exact-{img}/exact.elf"
    nm = subprocess.run([E.NM, "-n", str(elf)], capture_output=True, text=True, env=E.ENV).stdout
    syms, starts = {}, set()
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 3 and re.match(r"^[A-Za-z_]\w*$", p[2]):
            syms.setdefault(p[2], int(p[0], 16))
            if p[1] in "tTwW":
                starts.add(int(p[0], 16) & ~1)
    known = callseq.names(img)
    addr = {v: k for k, v in known.items()}.get(func, syms.get(func, 0) & ~1)
    if not addr:
        sys.exit(f"no address for {func}")
    end = min(b for b in starts | set(known) if b > addr)

    # target.o: original bytes at ADDR, pools marked as data
    binp = root / f"original/extracted/flippydrive-{img}.bin"
    segs = segments(binp.read_bytes(), int(P.BASE[img], 0))
    code = next(d[addr - b:end - b] for b, d in segs if b <= addr < b + len(d))
    pools = set()
    for l in E.disasm(code, addr):
        m = re.search(r"\[pc, #\d+\]\s*@ \((?:0x)?([0-9a-f]+)\)", l)
        if m:
            pools.add(int(m.group(1), 16) - addr)
    s = [".syntax unified", ".thumb", ".section .text.target,\"ax\",%progbits",
         f".global {func}", f".type {func}, %function", ".thumb_func", f"{func}:"]
    for k in range(0, len(code), 2):
        if any(p <= k < p + 4 for p in pools):
            if k in pools and k + 4 <= len(code):
                s.append(f".word 0x{int.from_bytes(code[k:k + 4], 'little'):08x}")  # $d
            continue
        s.append(f".inst.n 0x{int.from_bytes(code[k:k + 2], 'little'):04x}")  # $t
    (out / "target.s").write_text("\n".join(x for x in s if x) + f"\n.size {func}, .-{func}\n")
    # Calls from flash to RAM go through the original's veneers: point each such callee's
    # symbol at the veneer the original's BL uses, so the candidate links its BL there too.
    byaddr = {a & ~1: n for n, a in syms.items()}
    for k in range(0, len(code) - 3, 2):
        h, h2 = int.from_bytes(code[k:k + 2], "little"), int.from_bytes(code[k + 2:k + 4], "little")
        if h & 0xf800 == 0xf000 and h2 & 0xd000 == 0xd000 and not any(p <= k < p + 4 for p in pools):
            s_ = (h >> 10) & 1
            imm = ((-s_) << 24) | ((~((h2 >> 13) & 1) ^ s_) & 1) << 23 | ((~((h2 >> 11) & 1) ^ s_) & 1) << 22 \
                | (h & 0x3ff) << 12 | (h2 & 0x7ff) << 1
            t = addr + k + 4 + imm
            if t in byaddr or addr <= t < end:
                continue
            vb = next((d for b, d in segs if b <= t < b + len(d) - 16), None)
            base_ = next(b for b, d in segs if b <= t < b + len(d))
            for o in range(0, 16, 2):
                w = int.from_bytes(vb[t - base_ + o:t - base_ + o + 4], "little") if vb else 0
                if 0x20000000 <= w < 0x20042000 and w & 1 and (w & ~1) in byaddr:
                    syms[byaddr[w & ~1]] = t | 1
                    break
    lds = [f"{n} = 0x{a:08x};" for n, a in syms.items() if n != func]
    (out / "syms.ld").write_text("\n".join(lds) + "\n")
    (out / "link.ld").write_text(
        f"SECTIONS {{\n  .text 0x{addr:08x} : {{ *(.text.target) *(*.{func}) }}\n"
        f"  .other : {{ *(.text*) *(.time_critical*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) }}\n"
        f"  /DISCARD/ : {{ *(.ARM.*) *(.comment) }}\n}}\nINCLUDE {out}/syms.ld\n")
    gcc, ld = f"{TC}/bin/arm-none-eabi-gcc", f"{TC}/bin/arm-none-eabi-ld"
    env = f"export LD_LIBRARY_PATH={TC}/hostlib"
    # target.o: assemble the raw halfwords as code (force $t), then link at ADDR
    (out / "mk_target.sh").write_text(
        f"#!/bin/bash\n{env}\nset -e\ncd {out}\n"
        f"{gcc} -mcpu=cortex-m0plus -mthumb -c target.s -o target.raw.o\n"
        f"{ld} -T link.ld -o target.o target.raw.o\n")
    (out / "compile.sh").write_text(
        f"#!/bin/bash\n{env}\nset -e\n"
        f"{gcc} -I{out} -I{src.parent} {flags} -c \"$1\" -o \"$3.raw\"\n"
        f"{ld} --unresolved-symbols=ignore-all -T {out}/link.ld -o \"$3\" \"$3.raw\"\n")
    (out / "objdump.sh").write_text(f"#!/bin/bash\n{env}\nexec {E.OBJDUMP} -M force-thumb \"$@\"\n")
    for f in ("mk_target.sh", "compile.sh", "objdump.sh"):
        os.chmod(out / f, 0o755)
    pre = subprocess.run(f"{gcc} -I{src.parent} {flags} -E -P {src}", shell=True, capture_output=True,
                         text=True, env=dict(os.environ, LD_LIBRARY_PATH=f"{TC}/hostlib"))
    if pre.returncode:
        sys.exit(pre.stderr[-2000:])
    # decomp-permuter's C parser round trip breaks pico-sdk's inline helpers (attribute
    # placement, statement expressions), so the compiler gets the real preprocessed source
    # verbatim (PERM_IGNORE) and the parser a declarations-only copy (PERM_PRETEND).
    full = strip_bodies(pre.stdout, func)
    m = re.search(r"^[^\n;{}]*\b%s\s*\(" % re.escape(func), full, re.M)
    fstart = m.start()
    depth, j = 0, full.index("{", fstart)
    while True:
        depth += {"{": 1, "}": -1}.get(full[j], 0)
        j += 1
        if depth == 0:
            break
    target_fn = full[fstart:j]
    proto = target_fn[:target_fn.index("{")].rstrip() + ";"
    # attributes on the definition (noinline, section) do not change its code when compiled on
    # its own, and the permuter's printer moves them to where GCC rejects them
    hdr = target_fn[:target_fn.index("{")]
    while "__attribute__" in hdr:
        a = hdr.index("__attribute__")
        k, depth = hdr.index("(", a), 0
        while True:
            depth += {"(": 1, ")": -1}.get(hdr[k], 0)
            k += 1
            if depth == 0:
                break
        hdr = hdr[:a] + hdr[k:]
    target_fn = hdr + target_fn[target_fn.index("{"):]
    (out / "prelude.h").write_text(full[:fstart] + full[j:])  # no prototype: the permuter may retype
    decls = strip_bodies(full[:fstart] + proto + full[j:], "\0", keep_inline=False)
    (out / "base.c").write_text(f'PERM_IGNORE(\n#include "prelude.h"\n)\nPERM_PRETEND(\n{decls}\n)\n'
                                f"{target_fn}\n")
    (out / "settings.toml").write_text(
        f'func_name = "{func}"\ncompiler_type = "gcc"\nobjdump_command = "{out}/objdump.sh -drz"\n')
    r = subprocess.run(["bash", str(out / "mk_target.sh")], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[-2000:])
    r = subprocess.run(["bash", str(out / "compile.sh"), str(out / "base.c"), "-o", str(out / "base.o")],
                       capture_output=True, text=True)
    print(f"{out}: target {end - addr} bytes at 0x{addr:08x}; base.c compiles: "
          f"{'yes' if r.returncode == 0 else 'NO: ' + r.stderr[-500:]}")


if __name__ == "__main__":
    main()
