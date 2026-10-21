#!/bin/bash
# satrun.sh SET [limit_s]: every instance of a benchmark set, 4 at a time.
set=$1; lim=${2:-60}
find /tmp/satbench/$set -name '*.cnf' | sort | xargs -P3 -I{} python3 /tmp/satcheck.py {} $lim > /tmp/satres-$set.tsv
awk -F'\t' -v s=$set '{n++; ag[$7]++; ev[$6]++; kt+=$4; at+=$5; if ($5>mx) {mx=$5; mxf=$1}}
  END {printf "%-12s %4d instances | ", s, n; for (k in ag) printf "%s=%d ", k, ag[k]; printf "| "; for (k in ev) printf "%s=%d ", k, ev[k];
       printf "| kissat %.1fs total, aios %.1fs total, slowest aios %.2fs (%s)\n", kt, at, mx, mxf}' /tmp/satres-$set.tsv
