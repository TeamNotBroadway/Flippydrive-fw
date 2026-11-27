#!/usr/bin/env python3
"""Greedy search over branch hints in one function (run under WSL).

    python3 tools/hint_climb.py IMAGE SRC.c FUNC ADDR [--jobs N] [--write]

Each `if (...)` and `while (...)` condition in FUNC's body is a site. A site's state is plain,
expect-0 or expect-1; an existing likely()/unlikely() wrapper is stripped first and its state
is the starting state. Each round tries every single-site change in parallel and keeps the one
that raises (calls distance lowest, then hits) from tools/fnscore.py. Stops when nothing gains.
Prints the best score; --write puts the best variant into SRC.
"""
import os, re, shutil, subprocess, sys, tempfile, threading
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import permute as P


def body_span(text, func):
    s, e = P.find_body(text, func)
    return s, e


def sites(text, s, e):
    out = []
    for m in re.finditer(r"\b(?:if|while)\s*\(", text[s:e]):
        i = s + m.end()
        depth, j = 1, i
        while depth:
            depth += {"(": 1, ")": -1}.get(text[j], 0)
            j += 1
        out.append((i, j - 1))
    return out


WRAP = re.compile(r"^\s*(likely|unlikely)\s*\((.*)\)\s*$", re.S)


def normalise(text, func):
    """Strip likely/unlikely at sites; return text, sites and their starting states."""
    s, e = body_span(text, func)
    st = {}
    for a, b in reversed(sites(text, s, e)):
        m = WRAP.match(text[a:b])
        if m:
            inner = m.group(2)
            depth = 0  # only strip when the wrapper spans the whole condition
            ok = True
            for k, c in enumerate(inner):
                depth += {"(": 1, ")": -1}.get(c, 0)
                if depth < 0:
                    ok = False
                    break
            if ok and depth == 0:
                text = text[:a] + inner + text[b:]
    s, e = body_span(text, func)
    ss = sites(text, s, e)
    orig_s, orig_e = body_span(ORIG, func)
    for k, (a, b) in enumerate(sites(ORIG, orig_s, orig_e)):
        m = WRAP.match(ORIG[a:b])
        if m and k < len(ss):
            st[k] = 1 if m.group(1) == "likely" else 0
    return text, ss, st


def render(base, ss, state):
    t = base
    for k in sorted(state, reverse=True):
        a, b = ss[k]
        t = t[:a] + "__builtin_expect(!!(%s), %d)" % (t[a:b], state[k]) + t[b:]
    return t


def main():
    global ORIG
    img, src, func, addr = sys.argv[1:5]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 4
    src = os.path.abspath(src)
    ORIG = open(src).read()
    base, ss, state = normalise(ORIG, func)
    tmp = tempfile.mkdtemp(prefix="hint_")
    local = threading.local()

    def ev(st):
        if not hasattr(local, "d"):
            local.d = tempfile.mkdtemp(dir=tmp)
            for f in os.listdir(os.path.dirname(src)):
                if f.endswith((".c", ".h")):
                    shutil.copy(os.path.join(os.path.dirname(src), f), local.d)
        p = os.path.join(local.d, os.path.basename(src))
        open(p, "w").write(render(base, ss, st))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/fnscore.py"), img, p, func, addr],
                           capture_output=True, text=True)
        line = (r.stdout.splitlines() or [""])[0]
        m = re.match(r"\s*(\d+)/(\d+)", line)
        d = re.search(r"distance (\d+)", line)
        return (-(int(d.group(1)) if d else 999), int(m.group(1)) if m else -1), line

    cur, line = ev(state)
    print(f"{len(ss)} sites; start {line}", flush=True)
    while True:
        cands = []
        for k in range(len(ss)):
            for v in (0, 1, None):
                if state.get(k) == v:
                    continue
                ns = dict(state)
                if v is None:
                    ns.pop(k)
                else:
                    ns[k] = v
                cands.append(ns)
        with ThreadPoolExecutor(jobs) as ex:
            res = list(ex.map(ev, cands))
        i = max(range(len(cands)), key=lambda i: res[i][0])
        if res[i][0] <= cur:
            break
        state, (cur, line) = cands[i], res[i]
        print(f"  {line}", flush=True)
    print("best", line)
    if "--write" in sys.argv:
        open(src, "w").write(render(base, ss, state))
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
