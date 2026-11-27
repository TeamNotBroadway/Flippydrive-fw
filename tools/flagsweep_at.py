#!/usr/bin/env python3
"""Greedy compiler-flag search scored at located addresses only (run under WSL).

    python3 tools/flagsweep_at.py IMAGE SRC.c FLAGFILE [--jobs N] [--base "FLAGS"]

FLAGFILE holds one candidate flag (or space-separated group) per line. Each round adds, in
parallel trials, every remaining candidate to the kept set (passed as FD_CFLAGS after the
build flags, after --base) and keeps the best gain from lib_score.py. Stops when nothing gains.
"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def evaluate(img, src, flags):
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools/lib_score.py"), img, src],
                         capture_output=True, text=True,
                         env=dict(os.environ, FD_CFLAGS=" ".join(flags))).stdout
    m = re.search(r"windows (\d+)/\d+, exact (\d+)", out)
    return (int(m.group(2)), int(m.group(1))) if m else (-1, 0)


def main():
    img, src, ff = sys.argv[1:4]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 6
    cands = [l.strip() for l in open(ff) if l.strip() and not l.startswith("#")]
    base = sys.argv[sys.argv.index("--base") + 1].split() if "--base" in sys.argv else []
    kept, best = base, evaluate(img, src, base)
    print("start (exact, windows)", best, flush=True)
    while cands:
        with ThreadPoolExecutor(jobs) as ex:
            res = list(ex.map(lambda c: evaluate(img, src, kept + [c]), cands))
        c, s = max(zip(cands, res), key=lambda t: t[1])
        if s <= best:
            break
        kept.append(c)
        cands.remove(c)
        best = s
        print(f"  + {c} -> {s}", flush=True)
    print("kept:", " ".join(kept), best)


if __name__ == "__main__":
    main()
