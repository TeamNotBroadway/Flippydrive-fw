#!/usr/bin/env python3
"""Greedy config search scored at located addresses only (run under WSL).

    python3 tools/confsweep_at.py IMAGE SPEC.json [--jobs N]

SPEC is a tools/sweep/*.json file (src_dir, config, fixed, options). Each round tries every
single-option change in parallel, scoring with lib_score.py (windows of the functions listed in
syms/IMAGE.fatfs.tsv at their original addresses), and keeps the best gain. Unlike
confsweep.py --fuzzy, functions absent from the image cannot earn spurious windows, so options
that only add API functions score by their effect on the located ones (inlining).
"""
import json, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def evaluate(img, spec, cfg):
    with tempfile.TemporaryDirectory() as td:
        for n in os.listdir(spec["src_dir"]):
            if n.endswith((".c", ".h")):
                shutil.copy(os.path.join(spec["src_dir"], n), td)
        p = os.path.join(td, spec["config"])
        text = open(p, newline="").read()
        for k, v in cfg.items():
            text = re.sub(r"^(#define\s+%s\s+)\S+" % k, lambda m: m.group(1) + v, text, flags=re.M)
        open(p, "w", newline="").write(text)
        out = subprocess.run([sys.executable, os.path.join(ROOT, "tools/lib_score.py"), img,
                              os.path.join(td, "ff.c")], capture_output=True, text=True).stdout
    m = re.search(r"windows (\d+)/\d+, exact (\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else (-1, 0)


def main():
    img, spec = sys.argv[1], json.load(open(sys.argv[2]))
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 6
    cfg = dict(spec.get("fixed", {}))
    cfg.update({k: v[0] for k, v in spec["options"].items()})
    best = evaluate(img, spec, cfg)
    print("start", best, flush=True)
    while True:
        trials = [(k, v) for k, vals in spec["options"].items() for v in vals if cfg[k] != v]
        with ThreadPoolExecutor(jobs) as ex:
            res = list(ex.map(lambda kv: evaluate(img, spec, {**cfg, kv[0]: kv[1]}), trials))
        (k, v), s = max(zip(trials, res), key=lambda t: t[1])
        if s <= best:
            break
        cfg[k], best = v, s
        print(f"  {k}={v} -> {s}", flush=True)
    print(json.dumps({"score": best, "config": cfg}))


if __name__ == "__main__":
    main()
