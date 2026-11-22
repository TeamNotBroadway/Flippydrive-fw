#!/usr/bin/env python3
"""Call-order check for every function in syms/<image>.src.tsv (run under WSL).

    python3 tools/callseq_all.py [IMAGE ...]   ->  distance, function, file; mismatches only
"""
import os, pathlib, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P
import callseq

for img in sys.argv[1:] or ["main-ode", "bootloader"]:
    flags = P.build_flags(img)
    want = {}
    for line in (pathlib.Path(P.ROOT) / "syms" / f"{img}.src.tsv").read_text().splitlines():
        a, n = line.split("\t")
        want[n] = int(a, 16)
    binp = os.path.join(P.ROOT, f"original/extracted/flippydrive-{img}.bin")
    with tempfile.TemporaryDirectory() as td:
        for c in sorted(pathlib.Path(P.ROOT, img).glob("*.c")):
            o = os.path.join(td, c.stem + ".o")
            r = subprocess.run(f"{P.CC} -I{c.parent} {flags} -w -c {c} -o {o}", shell=True, cwd=c.parent,
                               capture_output=True)
            if r.returncode:
                print("compile failed", c)
                continue
            text = c.read_text()
            for n, a in want.items():
                if re.search(r"\b%s\b\)?\s*\([^;{]*\)\s*\{" % re.escape(n), text):
                    d, x, y = callseq.check(binp, int(P.BASE[img], 0), o, n, a, img)
                    if d:
                        print(f"{d:3} {img} {n} {c.name}", flush=True)
