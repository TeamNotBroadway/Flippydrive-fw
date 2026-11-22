#!/usr/bin/env python3
"""Score every job in permute_out/jobs.txt against the current sources (run under WSL).

    python3 tools/permute_score.py [FUNC ...]   ->  img func hits/total [EXACT]
"""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P

want = set(sys.argv[1:])
flags = {}
for line in open(os.path.join(P.ROOT, "permute_out/jobs.txt")):
    img, src, fn, addr, _ = line.split()
    if want and fn not in want:
        continue
    flags.setdefault(img, P.build_flags(img))
    path = os.path.join(P.ROOT, src)
    with tempfile.TemporaryDirectory() as td:
        sc = P.score(img, flags[img], open(path).read(), fn, addr, td, os.path.dirname(path))
    print(img, fn, f"{sc[0]}/{sc[1]}{' EXACT' if sc[2] else ''}" if sc else "FAIL", flush=True)
