#!/usr/bin/env python3
"""Write permute_out/jobs.txt: one permuter job per function the exact build still takes from
the original (run under WSL, after rp_exact.py).

    python3 tools/permute_jobs.py [--iters N]

Reads ~/fd-build/exact-<image>/status.tsv and finds each function's definition in <image>/*.c.
"""
import argparse, os, pathlib, re

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=800)
    a = ap.parse_args()
    jobs = []
    for img in ("bootloader", "main-ode"):
        status = pathlib.Path(os.path.expanduser(f"~/fd-build/exact-{img}/status.tsv"))
        srcs = {p: p.read_text() for p in sorted((ROOT / img).glob("*.c"))}
        for line in status.read_text().splitlines():
            addr, fn, kind = line.split("\t")[:3]
            if kind != "original":
                continue
            defn = re.compile(r"^[A-Za-z_][^;]*\b%s\b\)?\s*\([^;]*$" % re.escape(fn), re.M)
            where = [p for p, t in srcs.items() if defn.search(t)]
            if len(where) != 1:
                print(f"skip {img} {fn}: defined in {len(where)} files")
                continue
            jobs.append(f"{img} {where[0].relative_to(ROOT).as_posix()} {fn} {addr} {a.iters}")
    out = ROOT / "permute_out" / "jobs.txt"
    out.parent.mkdir(exist_ok=True)
    out.write_text("\n".join(jobs) + "\n")
    print(f"{len(jobs)} jobs in {out}")


if __name__ == "__main__":
    main()
