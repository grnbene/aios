#!/bin/bash
# $1 = file; prints name, kissat status, seconds (40 s cap)
f=$1; s=$(date +%s.%N)
timeout 40 /tmp/satref/kissat/build/kissat -q -n "$f" >/dev/null 2>&1; rc=$?
e=$(date +%s.%N); printf "%s\t%s\t%.2f\n" "$(basename $f)" $rc $(echo "$e - $s" | bc)
