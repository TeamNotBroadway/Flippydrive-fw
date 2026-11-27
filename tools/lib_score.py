#!/usr/bin/env python3
"""Score every located function of a library source against an image (run under WSL).

    python3 tools/lib_score.py IMAGE SRC.c [LIST.tsv]

LIST.tsv (default syms/IMAGE.fatfs.tsv) holds address, name per line. SRC.c is compiled once
with its build.ninja flags; each listed function is scored at its address. Prints one row per
function (windows, EXACT or first diff) and the totals.
"""
import os, pathlib, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P


def main():
    img, src = sys.argv[1], os.path.abspath(sys.argv[2])
    lst = pathlib.Path(sys.argv[3] if len(sys.argv) > 3 else os.path.join(P.ROOT, f"syms/{img}.fatfs.tsv"))
    at = {}
    for line in lst.read_text().splitlines():
        f = line.split("\t")
        if len(f) >= 2 and f[0].startswith("0x"):
            at[f[1]] = f[0]
    flags = P.build_flags(img, src)
    binp = os.path.join(P.ROOT, f"original/extracted/flippydrive-{img}.bin")
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, "f.o")
        r = subprocess.run(f"{P.CC} -I{os.path.dirname(src)} {flags} -w -c {src} -o {o}", shell=True,
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr[-3000:])
        out = subprocess.run([sys.executable, "-I", "funcdiff.py", binp, P.BASE[img], o],
                             env=dict(os.environ, AT=",".join(f"{k}={v}" for k, v in at.items())),
                             capture_output=True, text=True, cwd=os.path.join(P.ROOT, "tools")).stdout
    hits = total = exact = 0
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 4 and "/" in p[0] and p[-1] in at:
            print(l)
            a, b = map(int, p[0].split("/"))
            hits, total, exact = hits + a, total + b, exact + ("EXACT" in l)
    print(f"{len(at)} listed: windows {hits}/{total}, exact {exact}")


if __name__ == "__main__":
    main()
