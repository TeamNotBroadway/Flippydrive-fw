"""Find library functions in the original images and name them.

For every STT_FUNC symbol in the given objects/archives, take its bytes with relocated
words masked out and search the image (flash plus the regions crt0 copies to RAM).
Writes one line per match: address, name, size, number of places it matched, source.

Usage: symmatch.py IMAGE.bin BASE OUT.tsv (OBJ_DIR | LIB.a | OBJ.o)...
A .dol IMAGE is searched by its sections (BASE is ignored); PowerPC code is 4-aligned.
"""
import collections
import pathlib
import re
import struct
import sys


# Functions with fewer non-relocated bytes than this match too many places to name anything.
MIN_FIXED = 10


def ar_members(data):
    o = 8
    names = b''
    while o < len(data):
        n = data[o:o + 16]
        size = int(data[o + 48:o + 58])
        body = data[o + 60:o + 60 + size]
        if n.startswith(b'//'):
            names = body
        elif n.startswith(b'/') and n[1:2].isdigit():
            i = int(n[1:].strip())
            yield names[i:names.index(b'/\n', i)].decode(), body
        elif not n.startswith(b'/ '):
            yield n.split(b'/')[0].decode(), body
        o += 60 + size + (size & 1)


# PowerPC relocation types that patch only a halfword at r_offset (ADDR16_LO/HI/HA,
# SDAREL16, ADDR16); everything else is masked as a whole word.
PPC_HALF = {3, 4, 5, 6, 32}


def elf_functions(elf):
    """Yield (name, code, mask) for each sized function symbol in an ELF relocatable
    (ARM little-endian with REL, or PowerPC big-endian with RELA)."""
    e = '<' if elf[5] == 1 else '>'
    ppc = struct.unpack_from(e + 'H', elf, 0x12)[0] == 20
    shoff, = struct.unpack_from(e + 'I', elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(e + 'HHH', elf, 0x2e)
    sh = [struct.unpack_from(e + '10I', elf, shoff + i * shentsize) for i in range(shnum)]

    def cstr(off):
        return elf[off:elf.index(b'\0', off)].decode()

    masks = {}
    for s in sh:
        if s[1] in (4, 9):  # SHT_RELA, SHT_REL
            target = s[7]
            code_sz = sh[target][5]
            m = masks.setdefault(target, bytearray(b'\1' * code_sz))
            ent = 12 if s[1] == 4 else 8
            for k in range(s[5] // ent):
                ro, info = struct.unpack_from(e + 'II', elf, s[4] + k * ent)
                width = 2 if ppc and info & 0xff in PPC_HALF else 4
                for b in range(width):
                    if ro + b < code_sz:
                        m[ro + b] = 0
    for s in sh:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sh[s[6]][4]
        syms = [struct.unpack_from(e + 'IIIBBH', elf, s[4] + k * 16) for k in range(s[5] // 16)]
        # PowerPC assembly (libogc's .S files) declares untyped, unsized labels: size each
        # one as the distance to the next symbol in its section.
        starts = collections.defaultdict(set)
        for st_name, value, _, info, _, shndx in syms:
            if info & 0xf in (0, 2) and 0 < shndx < 0xff00:
                starts[shndx].add(value)
        for st_name, value, size, info, _, shndx in syms:
            if ppc and size == 0 and info & 0xf == 0 and 0 < shndx < 0xff00 and st_name \
                    and not cstr(strtab + st_name).startswith(('.L', '$')):
                later = [v for v in starts[shndx] if v > value]
                size = (min(later) if later else sh[shndx][5]) - value
                info = (info & 0xf0) | 2
            if info & 0xf != 2 or size < 8 or shndx == 0 or shndx >= 0xff00:
                continue
            sec = sh[shndx]
            if not sec[2] & 4:  # SHF_EXECINSTR
                continue
            start = value & ~1
            code = elf[sec[4] + start:sec[4] + start + size]
            mask = bytes(masks.get(shndx, b'\1' * sec[5])[start:start + size])
            yield cstr(strtab + st_name), code, mask


def sources(paths):
    for p in map(pathlib.Path, paths):
        if p.is_dir():
            for o in sorted(p.rglob('*.o')) + sorted(p.rglob('*.obj')):
                yield o.as_posix(), o.read_bytes()
        elif p.suffix == '.a':
            for mname, body in ar_members(p.read_bytes()):
                if body[:4] == b'\x7fELF':
                    yield f'{p.name}:{mname}', body
        else:
            yield p.as_posix(), p.read_bytes()


def dol_segments(img):
    """The text and data sections of a GameCube DOL, as (address, bytes)."""
    offs = struct.unpack_from('>18I', img, 0)
    addrs = struct.unpack_from('>18I', img, 0x48)
    sizes = struct.unpack_from('>18I', img, 0x90)
    return [(addrs[i], img[offs[i]:offs[i] + sizes[i]]) for i in range(18) if sizes[i]]


def segments(img, base):
    """Each RAM region from crt0's data_cpy_table, plus the flash outside their load images."""
    segs = []
    holes = []
    bi = img.find(struct.pack('<I', 0x7188ebf2))
    table = struct.unpack_from('<I', img, bi + 12)[0] - base
    while True:
        src, dst, end = struct.unpack_from('<III', img, table)
        if src == 0:
            break
        if end > dst:
            segs.append((dst, img[src - base:src - base + end - dst]))
            holes.append((src - base, src - base + end - dst))
        table += 12
    pos = 0
    for a, b in sorted(holes):
        segs.append((base + pos, img[pos:a]))
        pos = b
    segs.append((base + pos, img[pos:]))
    return segs


def main():
    img = pathlib.Path(sys.argv[1]).read_bytes()
    base = int(sys.argv[2], 0)
    segs = dol_segments(img) if sys.argv[1].endswith('.dol') else segments(img, base)
    seen = {}
    align = 4 if sys.argv[1].endswith('.dol') else 2
    for src, elf in sources(sys.argv[4:]):
        for name, code, mask in elf_functions(elf):
            if sum(mask) < MIN_FIXED:
                continue
            key = (code, mask)
            if key in seen:
                seen[key][1].append((name, src))
                continue
            pat = re.compile(b''.join(re.escape(bytes([c])) if m else b'.'
                                      for c, m in zip(code, mask)), re.S)
            hits = [sb + m.start() for sb, data in segs for m in pat.finditer(data)
                    if m.start() % align == 0]
            seen[key] = (hits, [(name, src)])
    by_addr = collections.defaultdict(list)
    for (code, _), (hits, names) in seen.items():
        for a in hits:
            by_addr[a].append((len(code), len(hits), names))
    with open(sys.argv[3], 'w') as f:
        for a in sorted(by_addr):
            size, count, names = max(by_addr[a], key=lambda t: t[0])
            uniq = sorted({n for n, _ in names})
            f.write(f'0x{a:08x}\t{"|".join(uniq)}\t{size}\t{count}\t{names[0][1]}\n')
    print(sys.argv[1], 'matched addresses:', len(by_addr))


if __name__ == '__main__':
    main()
