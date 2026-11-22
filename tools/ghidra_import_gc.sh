#!/bin/bash
# Import the GameCube images (ELF wrappers written by tools/gcdisc.py) into the Ghidra project
# ghidra/, apply library symbols from syms/gc-<image>.lib.tsv when present, analyse, and
# export decompiled C to decomp/gc-<image>/. Run from Git Bash on Windows.
set -e
cd "$(dirname "$0")/.."
export JAVA_HOME='C:\Tools\openjdk-26.0.2_windows-x64_bin\jdk-26.0.2'
G='C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
mkdir -p ghidra decomp
for n in "${@:-apploader main payload}"; do
  for m in $n; do
    cmd //c "$G" ghidra flippydrive -overwrite \
        -import "original/extracted/gc/$m.elf" -processor PowerPC:BE:32:default \
        -scriptPath tools/ghidra \
        -preScript GcSetup.java "syms/gc-$m.lib.tsv" \
        -postScript FdExport.java "decomp/gc-$m"
  done
done
