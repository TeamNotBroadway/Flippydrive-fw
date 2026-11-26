#!/usr/bin/env python3
"""Score one function of a source file against the original (run under WSL).

    python3 tools/fnscore.py IMAGE SRC.c FUNC ADDR [--show]

Compiles SRC.c with its flags from build.ninja (a copy elsewhere, e.g. a workspace ff.c, takes
the flags of the tree file with the same name), then prints matched 8-byte windows at ADDR,
EXACT when every unrelocated byte matches, and the call-order distance (0 = same calls in the
same order). --show adds our disassembly beside the original's, '*' on differing lines.
"""
import argparse, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P
import callseq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("img"); ap.add_argument("src"); ap.add_argument("func"); ap.add_argument("addr")
    ap.add_argument("--show", action="store_true")
    a = ap.parse_args()
    src = os.path.abspath(a.src)
    flags = P.build_flags(a.img, src)
    binp = os.path.join(P.ROOT, f"original/extracted/flippydrive-{a.img}.bin")
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, "f.o")
        r = subprocess.run(f"{P.CC} -I{os.path.dirname(src)} {flags} -c {src} -o {o}", shell=True,
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr[-3000:])
        dist, ours, theirs = callseq.check(binp, int(P.BASE[a.img], 0), o, a.func, int(a.addr, 0), a.img)
        cmd = [sys.executable, "-I", "funcdiff.py", binp, P.BASE[a.img], o]
        if a.show:
            cmd += ["--show", a.func]
        out = subprocess.run(cmd, env=dict(os.environ, AT=f"{a.func}={a.addr}"), capture_output=True,
                             text=True, cwd=os.path.join(P.ROOT, "tools")).stdout
        lines = out.splitlines()
        row = next((l for l in lines if l.split()[-1:] == [a.func] and "/" in l.split()[0]), None)
        if row is None:
            sys.exit(f"{a.func} not emitted (inlined or renamed: try its .isra/.part/.constprop name)")
        print(row.strip(), f" calls distance {dist}")
        if dist:
            for i in range(max(len(ours), len(theirs))):
                x = ours[i] if i < len(ours) else "-"
                y = theirs[i] if i < len(theirs) else "-"
                print(f"   {' ' if callseq.same(x, y) else '*'} {x:28} {y[:50]}")
        if a.show:
            print("\n".join(l for l in lines if l[:2] in ("  ", "* ")))


if __name__ == "__main__":
    main()
