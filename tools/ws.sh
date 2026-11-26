#!/bin/bash
# Per-function workspaces for library decompilation (run in WSL). Each workspace is a private
# copy of a library's sources, so several people or agents can edit at once.
#
#   bash tools/ws.sh init  LIB FUNC          copy third_party/LIB/*.[ch] to permute_out/ws/FUNC/
#   bash tools/ws.sh score FUNC ADDR [--show] score FUNC in the workspace's source (tools/fnscore.py)
#   bash tools/ws.sh permute FUNC ADDR [ITERS] permuter on the workspace (best: permute_out/FUNC.best.c)
#
# The workspace source is the file in it that defines FUNC.
set -e
cd "$(dirname "$0")/.."
cmd=$1; shift
case $cmd in
init)
    d=permute_out/ws/$2; mkdir -p "$d"; cp third_party/$1/*.c third_party/$1/*.h "$d"/; echo "$d";;
score|permute)
    f=$1; d=permute_out/ws/$f
    src=$(grep -lE "^[A-Za-z_][^;]*\b$f\b\)?\s*\(" "$d"/*.c | head -1)
    [ -n "$src" ] || { echo "no definition of $f in $d"; exit 1; }
    if [ $cmd = score ]; then python3 tools/fnscore.py main-ode "$src" "$f" "$2" ${3:-}
    else python3 tools/permute.py main-ode "$src" "$f" "$2" --iters ${3:-300} --jobs 4 2>/dev/null | tail -1; fi;;
*) sed -n 2,10p "$0"; exit 1;;
esac
