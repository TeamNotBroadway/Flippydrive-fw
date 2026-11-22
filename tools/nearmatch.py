"""Name library functions that are close to, but not byte-identical with, code in an image.

For libraries the original built from modified sources (FatFs, esp-serial-flasher): each
function's non-relocated 8-byte windows are looked up in the image, and the start address
most windows agree on is taken as its location. Appends symmatch.py-format lines to OUT.tsv
for addresses it does not already name, with source "near:HITS/TOTAL:VOTES". Run in WSL.

Usage: nearmatch.py IMAGE.bin BASE OUT.tsv (LIB.a | OBJ.o)...
(IMAGE.dol: searched by DOL sections, BASE ignored)
"""
import collections
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from symmatch import elf_functions, segments, sources, dol_segments  # noqa: E402

W = 8
MIN_VOTES = 3       # windows agreeing on the start address
MIN_SHARE = 0.15    # of the function's windows found anywhere in the image


def locate(index, code, mask):
    """Return (start, hits, total, votes) for the best-supported start address, or None."""
    votes = collections.Counter()
    total = hits = 0
    for i in range(0, len(code) - W + 1, 2):
        if not all(mask[i:i + W]):
            continue
        total += 1
        locs = index.get(code[i:i + W], ())
        if locs:
            hits += 1
            if len(locs) <= 4:
                for a in locs:
                    votes[a - i] += 1
    if not votes:
        return None
    start, n = votes.most_common(1)[0]
    return start, hits, total, n


def main():
    img = pathlib.Path(sys.argv[1]).read_bytes()
    out = pathlib.Path(sys.argv[3])
    index = collections.defaultdict(list)
    dol = sys.argv[1].endswith('.dol')
    align = 4 if dol else 2
    for base, data in (dol_segments(img) if dol else segments(img, int(sys.argv[2], 0))):
        for i in range(0, len(data) - W + 1, 2):
            index[data[i:i + W]].append(base + i)
    named = {int(l.split('\t')[0], 16) for l in out.read_text().splitlines()} if out.exists() else set()
    best = {}
    for src, elf in sources(sys.argv[4:]):
        for name, code, mask in elf_functions(elf):
            r = locate(index, code, mask)
            if not r:
                continue
            start, hits, total, n = r
            if n < MIN_VOTES or hits < MIN_SHARE * total or start in named or start % align:
                continue
            if start not in best or n > best[start][3]:
                best[start] = (name, len(code), f'near:{hits}/{total}:{n}', n)
    with out.open('a') as f:
        for a in sorted(best):
            name, size, how, _ = best[a]
            f.write(f'0x{a:08x}\t{name}\t{size}\t1\t{how}\n')
    print(sys.argv[1], 'near matches:', len(best))


if __name__ == '__main__':
    main()
