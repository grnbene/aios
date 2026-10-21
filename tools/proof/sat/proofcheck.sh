#!/bin/bash
# $1 = solver binary; rest = cnf files. Every UNSAT proof through drat-trim, every SAT model checked by the CLI.
S=$1; shift; D=/tmp/satref/drat-trim/drat-trim; n=0; bad=0; viv=0
for f in "$@"; do
  p=/tmp/pc-$$.drat
  o=$(taskset -c ${CORE:-3} $S $f --threads=1 --proof=$p); st=$(echo "$o" | grep "^s " | head -1)
  vl=$(echo "$o" | grep -o 'vivify_lits=[0-9]*' | cut -d= -f2); viv=$((viv+${vl:-0}))
  if [ "$st" = "s UNSATISFIABLE" ]; then
    v=$(taskset -c ${CORE:-3} $D $f $p 2>&1 | tr -d "\r" | grep "^s " | head -1); n=$((n+1))
    [ "$v" = "s VERIFIED" ] || { bad=$((bad+1)); echo "BAD $(basename $f): $v"; }
  elif [ "$st" != "s SATISFIABLE" ]; then bad=$((bad+1)); echo "BAD $(basename $f): $st"; fi
  rm -f $p
done
echo "proofs checked=$n bad=$bad vivified_literals=$viv"
