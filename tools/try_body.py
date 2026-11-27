#!/usr/bin/env python3
"""Score replacement bodies of one function across a whole library (run under WSL).

    python3 tools/try_body.py IMAGE SRC.c FUNC VARIANTS [LIST.tsv]

VARIANTS is a text file of function bodies (the text between the outer braces) separated by
lines holding only '%%'. Each variant replaces FUNC's body in a temporary copy of SRC.c's
directory; lib_score.py then scores every listed function. Prints the baseline and each
variant's totals, plus the functions whose windows changed. For helpers (ld_word, ld_clust)
whose shape shows up inlined in many callers, where per-function workspaces cannot reach.
"""
import os, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import permute as P


def score(src, lst):
    r = subprocess.run([sys.executable, os.path.join(P.ROOT, "tools/lib_score.py"), sys.argv[1], src]
                       + ([lst] if lst else []), capture_output=True, text=True)
    rows, total = {}, r.stdout.strip().splitlines()[-1:] or [r.stderr[-500:]]
    for l in r.stdout.splitlines()[:-1]:
        p = l.split()
        rows[p[-1]] = int(p[0].split("/")[0])
    return rows, total[0]


def main():
    img, src, func, var = sys.argv[1:5]
    lst = sys.argv[5] if len(sys.argv) > 5 else None
    text = open(src, newline="").read()
    # The definition: FUNC on a line starting at column 0 (P.find_body can match a call site).
    m = re.search(r"^[A-Za-z_][^;\n]*\b%s\s*\([^;{]*\{" % re.escape(func), text, re.M)
    s = e = m.end() - 1
    depth = 0
    for e in range(s, len(text)):
        depth += {"{": 1, "}": -1}.get(text[e], 0)
        if depth == 0:
            break
    e += 1
    bodies = [b.strip("\n") for b in re.split(r"^%%\s*$", open(var).read(), flags=re.M)]
    with tempfile.TemporaryDirectory() as td:
        for n in os.listdir(os.path.dirname(os.path.abspath(src))):
            if n.endswith((".c", ".h")):
                shutil.copy(os.path.join(os.path.dirname(os.path.abspath(src)), n), td)
        t = os.path.join(td, os.path.basename(src))
        base, btot = score(t, lst)
        print("base:", btot)
        for i, b in enumerate(bodies):
            open(t, "w", newline="").write(text[:s] + "{\n" + b + "\n}" + text[e:])
            rows, tot = score(t, lst)
            if not rows:
                print(f"v{i}: compile failed: {tot.strip().splitlines()[-1][:200]}")
                continue
            ch = " ".join(f"{k}{rows.get(k, 0) - v:+d}" for k, v in base.items() if rows.get(k, 0) != v)
            print(f"v{i}: {tot}  {ch}")


if __name__ == "__main__":
    main()
