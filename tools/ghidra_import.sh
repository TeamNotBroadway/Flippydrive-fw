#!/bin/bash
# Import both originals into a Ghidra project (ghidra/, not in git), apply library symbols
# from syms/*.lib.tsv, analyse, and export decompiled C to decomp/<image>/.
# Run from Git Bash on Windows.
set -e
cd "$(dirname "$0")/.."
export JAVA_HOME='C:\Tools\openjdk-26.0.2_windows-x64_bin\jdk-26.0.2'
G='C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
mkdir -p ghidra decomp
for t in bootloader:0x10000000 main-ode:0x10080000; do
    n=${t%%:*}; b=${t#*:}
    cmd //c "$G" ghidra flippydrive -overwrite \
        -import "original/extracted/flippydrive-$n.bin" \
        -loader BinaryLoader -loader-baseAddr $b -processor ARM:LE:32:Cortex \
        -scriptPath tools/ghidra \
        -preScript FdSetup.java "syms/$n.lib.tsv" \
        -postScript FdExport.java "decomp/$n"
done
