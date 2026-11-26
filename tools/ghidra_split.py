#!/usr/bin/env python3
"""Split FdDecompAt.java's log into one file per function: ghidra_split.py LOG OUTDIR."""
import pathlib, re, sys

log, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
cur, buf, n = None, [], 0
for line in log.read_text(errors="replace").splitlines():
    line = re.sub(r"^INFO\s+FdDecompAt\.java>\s?", "", line)   # headless println prefix
    line = re.sub(r"\s+\(GhidraScript\)\s*$", "", line)
    m = re.match(r"==== (0x[0-9a-fA-F]+) .*====$", line)
    if m and not line.startswith("==== end"):
        cur, buf = m.group(1).lower(), []
        continue
    if line.startswith("==== end") and cur:
        (out / f"{cur}.c").write_text("\n".join(buf).strip() + "\n")
        cur, n = None, n + 1
        continue
    if cur:
        buf.append(line)
print(f"{n} functions written to {out}")
