"""Per-function closeness of compiled objects to an image, for finding why near-misses differ.

For each function: share of its non-relocated 8-byte windows found in the image, the image
address most windows agree on, and the first differing byte offset there. With --show NAME,
disassembles that function and the image at the voted address side by side. Run in WSL.

Usage: funcdiff.py IMAGE.bin BASE OBJ... [--show NAME]
(IMAGE.dol: PowerPC, searched by DOL sections, BASE ignored)
"""
import collections
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from symmatch import elf_functions, segments, dol_segments  # noqa: E402

import os
OBJDUMP = os.environ.get('OBJDUMP', '/mnt/c/Tools/arch-arm-none-eabi-14.2.0/bin/arm-none-eabi-objdump')
PPC_OBJDUMP = '/mnt/c/Tools/devkitppc-20250727/root/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump'
PPC = False
W = 8
import os
FORCE = {k: int(v, 16) for k, v in (x.split("=") for x in os.environ.get("AT", "").split(",") if x)}


def disasm(code, addr):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(code)
    arch = ['-mpowerpc:750', '-EB'] if PPC else ['-marm', '-Mforce-thumb']
    out = subprocess.run([PPC_OBJDUMP if PPC else OBJDUMP, '-D', '-b', 'binary'] + arch +
                         [f'--adjust-vma=0x{addr:x}', f.name], capture_output=True, text=True,
                         env={'LD_LIBRARY_PATH': os.path.join(os.path.dirname(os.path.dirname(OBJDUMP)), 'hostlib')})
    return [l for l in out.stdout.splitlines() if l.startswith('  ') or l[:1].isdigit()][1:]


def main():
    args = sys.argv[1:]
    show = None
    if '--show' in args:
        i = args.index('--show')
        show = args[i + 1]
        del args[i:i + 2]
    global PPC
    img = pathlib.Path(args[0]).read_bytes()
    PPC = args[0].endswith('.dol')
    segs = dol_segments(img) if PPC else segments(img, int(args[1], 0))
    index = collections.defaultdict(list)
    for base, data in segs:
        for i in range(0, len(data) - W + 1, 2):
            index[data[i:i + W]].append(base + i)

    def byte_at(a):
        for base, data in segs:
            if base <= a < base + len(data):
                return data[a - base]
        return None

    rows = []
    for obj in args[2:]:
        for name, code, mask in elf_functions(pathlib.Path(obj).read_bytes()):
            votes = collections.Counter()
            total = hits = 0
            for i in range(0, len(code) - W + 1, 2):
                if not all(mask[i:i + W]):
                    continue
                total += 1
                locs = index.get(code[i:i + W], [])
                if locs:
                    hits += 1
                    if len(locs) <= 4:
                        for a in locs:
                            votes[a - i] += 1
            start = votes.most_common(1)[0][0] if votes else None
            if FORCE.get(name) is not None: start = FORCE[name]
            first = None
            if start is not None:
                for k in range(len(code)):
                    if mask[k] and byte_at(start + k) != code[k]:
                        first = k
                        break
            rows.append((name, len(code), total, hits, start, first, code))
    for name, size, total, hits, start, first, code in sorted(rows, key=lambda r: -r[3] / max(r[2], 1)):
        loc = f'0x{start:08x}' if start is not None else '-'
        state = 'EXACT' if start is not None and first is None else f'first diff +0x{first:x}' if first is not None else ''
        print(f'{hits:4}/{total:<4} {size:5}  {loc}  {state:18} {name}')
        if name == show and start is not None:
            ours = disasm(code, start)
            theirs = disasm(bytes(byte_at(start + k) or 0 for k in range(size + 16)), start)
            for a, b in zip(ours, theirs):
                mark = ' ' if a.split('\t')[1:2] == b.split('\t')[1:2] else '*'
                print(f'{mark} {a[:60]:60} | {b[:60]}')


if __name__ == '__main__':
    main()
