#!/usr/bin/env python3
"""Apply permute_out/<func>.best.c bodies to the sources where they improve the score.

    python3 tools/permute_apply.py [FUNC ...]      (run under WSL)

For each job in permute_out/jobs.txt it swaps FUNC's body from the best variant into the
real source, rescores the whole file, and keeps the change only if FUNC improves and no other
function in that file loses windows. Prints a unified diff of every kept change for review.
"""
import difflib, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P

ROOT = P.ROOT


def file_scores(img, flags, src, addrs, tmp, srcdir):
    out = {}
    for fn, addr in addrs.items():
        out[fn] = P.score(img, flags, src, fn, addr, tmp, srcdir)
    return out


def main():
    want = set(sys.argv[1:])
    jobs = [l.split() for l in open(os.path.join(ROOT, "permute_out/jobs.txt"))]
    byfile = {}
    for img, src, fn, addr, _ in jobs:
        byfile.setdefault((img, src), {})[fn] = addr
    import tempfile
    for (img, src), addrs in byfile.items():
        flags = P.build_flags(img)
        path = os.path.join(ROOT, src)
        srcdir = os.path.dirname(path)
        with tempfile.TemporaryDirectory() as td:
            cur = open(path).read()
            base = file_scores(img, flags, cur, addrs, td, srcdir)
            for fn in addrs:
                if want and fn not in want:
                    continue
                best = os.path.join(ROOT, "permute_out", f"{fn}.best.c")
                if not os.path.exists(best) or base.get(fn) is None:
                    continue
                b = open(best).read()
                try:
                    bs, be = P.find_body(b, fn)
                    cs, ce = P.find_body(cur, fn)
                except SystemExit:
                    continue
                if b[bs:be] == cur[cs:ce]:
                    continue
                cand = cur[:cs] + b[bs:be] + cur[ce:]
                sc = file_scores(img, flags, cand, addrs, td, srcdir)
                if sc[fn] is None or P.key(sc[fn]) <= P.key(base[fn]):
                    print(f"skip {fn}: {base[fn]} -> {sc[fn]}")
                    continue
                if any(sc[o] is None or P.key(sc[o]) < P.key(base[o]) for o in addrs if o != fn and base[o]):
                    print(f"skip {fn}: another function regresses")
                    continue
                print(f"KEEP {fn}: {base[fn][0]}/{base[fn][1]} -> {sc[fn][0]}/{sc[fn][1]}"
                      f"{' EXACT' if sc[fn][2] else ''}")
                sys.stdout.writelines(difflib.unified_diff(
                    cur[cs:ce].splitlines(True), b[bs:be].splitlines(True), f"{src}:{fn}", "best", n=1))
                cur, base = cand, sc
            with open(path, "w", newline="\n") as f:
                f.write(cur)


if __name__ == "__main__":
    main()
