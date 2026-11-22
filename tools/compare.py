"""Compare a built image with the original. Prints the match count and mismatching ranges.
Usage: compare.py BUILT.bin ORIGINAL.bin BASE_ADDR [MAX_RANGES]"""
import sys

built, orig = (open(p, 'rb').read() for p in sys.argv[1:3])
base = int(sys.argv[3], 0)
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 20
n = max(len(built), len(orig))
same = sum(1 for i in range(min(len(built), len(orig))) if built[i] == orig[i])
print(f'built 0x{len(built):x} bytes, original 0x{len(orig):x}; {same}/{len(orig)} bytes equal'
      f'{" -- IDENTICAL" if built == orig else ""}')
ranges, start = [], None
for i in range(n):
    diff = i >= len(built) or i >= len(orig) or built[i] != orig[i]
    if diff and start is None:
        start = i
    elif not diff and start is not None:
        ranges.append((start, i)); start = None
if start is not None:
    ranges.append((start, n))
for a, b in ranges[:limit]:
    print(f'  differ 0x{base + a:08x}-0x{base + b:08x} ({b - a} bytes)')
if len(ranges) > limit:
    print(f'  ... {len(ranges) - limit} more ranges')
