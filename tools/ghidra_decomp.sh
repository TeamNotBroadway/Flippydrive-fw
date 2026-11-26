#!/bin/bash
# Decompile functions of an original image with Ghidra, one file per function.
# Run from Git Bash on Windows, after tools/ghidra_import.sh.
#
#   bash tools/ghidra_decomp.sh IMAGE OUTDIR ADDRFILE
#
# ADDRFILE holds one address per line (first column; tools/fn_starts.py writes it). Each
# function is written to OUTDIR/<address>.c. tools/ghidra/FdDecompAt.java re-creates the
# function at the exact address, so a wrong Ghidra boundary does not leak into the result;
# the project is opened read-only and not changed.
set -e
cd "$(dirname "$0")/.."
img=$1; out=$2; list=$3
export JAVA_HOME='C:\Tools\openjdk-26.0.2_windows-x64_bin\jdk-26.0.2'
G='C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
mkdir -p "$out"
log="$out/ghidra.log"
cmd //c "$G" ghidra flippydrive -process "flippydrive-$img.bin" -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript FdDecompAt.java $(cut -f1 "$list" | tr '\n' ' ') > "$log" 2>&1
python -I tools/ghidra_split.py "$log" "$out"
