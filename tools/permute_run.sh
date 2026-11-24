#!/bin/bash
# Run every job in permute_out/jobs.txt through tools/permute.py (run in WSL; meant for the
# background). Each job's last line goes to permute_out/results.txt; the best variants are
# permute_out/<func>.best.c. Review and apply them with tools/permute_apply.py.
#
#   bash tools/permute_run.sh [JOBS_PER_FUNCTION]      (default 6 parallel compiles)
cd "$(dirname "$0")/.."
J=${1:-6}
: > permute_out/results.txt
while read img src fn addr it; do
    echo "$fn $(python3 tools/permute.py $img $src $fn $addr --iters $it --jobs $J 2>/dev/null | tail -1)" \
        >> permute_out/results.txt
done < permute_out/jobs.txt
echo DONE >> permute_out/results.txt
