"""Compare two GameCube DOLs: section addresses and sizes, then differing byte ranges.

Usage: dolcmp.py ORIGINAL.dol BUILT.dol [MAX_RANGES]
Exits 0 when the files are identical.
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0].rsplit('\\', 1)[0])
from symmatch import dol_segments  # noqa: E402
import struct  # noqa: E402


def main():
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    if a == b:
        print('identical')
        return 0
    sa, sb = dol_segments(a), dol_segments(b)
    print('original', [(hex(v), hex(len(d))) for v, d in sa],
          'bss/entry', [hex(x) for x in struct.unpack_from('>3I', a, 0xd8)])
    print('built   ', [(hex(v), hex(len(d))) for v, d in sb],
          'bss/entry', [hex(x) for x in struct.unpack_from('>3I', b, 0xd8)])
    for (va, da), (vb, db) in zip(sa, sb):
        if va != vb:
            continue
        ranges = []
        i, n = 0, min(len(da), len(db))
        while i < n:
            if da[i] != db[i]:
                j = i
                while j < n and (da[j] != db[j] or da[j:j + 16] != db[j:j + 16]):
                    j += 1
                ranges.append((va + i, j - i))
                i = j
            i += 1
        same = n - sum(r[1] for r in ranges)
        print(f'section {va:08x}: {same}/{n} bytes equal, {len(ranges)} differing ranges')
        for r, length in ranges[:limit]:
            print(f'  {r:08x} +{length:x}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
