#!/usr/bin/env bash
# grain against PHP on the same work: tools/grain-vs-php/run.sh [grain binary] [runs]
# Each file is run `runs` times (default 5); the table is the median per line, and grain / PHP (under 1: grain faster).
# Both sides must print the same result column, or the line says so: a faster wrong answer is not faster.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
grain="${1:-$here/../../build-grain/grain}"
runs="${2:-5}"
command -v php >/dev/null || { echo "php is not installed"; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
for set in workloads ops; do
  for i in $(seq "$runs"); do
    "$grain" "$here/$set.gr" > "$tmp/g.$set.$i"
    php "$here/$set.php" > "$tmp/p.$set.$i"
  done
  echo
  echo "| $set | grain ms | PHP ms | grain / PHP |"
  echo "|---|---|---|---|"
  cut -f1 "$tmp/g.$set.1" | while IFS= read -r name; do
    g=$(for f in "$tmp"/g.$set.*; do awk -F'\t' -v n="$name" '$1==n{print $2}' "$f"; done | median)
    p=$(for f in "$tmp"/p.$set.*; do awk -F'\t' -v n="$name" '$1==n{print $2}' "$f"; done | median)
    gr=$(awk -F'\t' -v n="$name" '$1==n{print $3}' "$tmp/g.$set.1")
    pr=$(awk -F'\t' -v n="$name" '$1==n{print $3}' "$tmp/p.$set.1")
    ratio=$(awk -v g="$g" -v p="$p" 'BEGIN { printf (p > 0 ? "%.2f" : "-"), g / (p > 0 ? p : 1) }')
    note=""; [ "$gr" != "$pr" ] && note=" (results differ: $gr vs $pr)"
    echo "| $name | $g | $p | $ratio$note |"
  done
done
