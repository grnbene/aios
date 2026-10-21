#!/bin/bash
# usage: knob.sh "ENV=.. ENV2=.." ; prints per-instance status/conflicts/time and totals (cap 40 s)
E="$1"; tot=0; solved=0
for h in dd169198070f 246afd75cb97 a4b05fbc5be2 b09585f2346c 37d40a1092b5 4fdcf4248a33 e6d247292b52 2d0c041c0fe7; do
  f=$(ls /nvme/tmp/sc2023/$h*.cnf)
  o=$(env $E taskset -c ${CORE:-3} /tmp/aios-sat-k $f --threads=1 --seconds=40 | grep -E "^s |^c vars")
  st=$(echo "$o" | grep "^s " | cut -c3-9); c=$(echo "$o" | grep -o "conflicts=[0-9]*" | cut -d= -f2); t=$(echo "$o" | grep -o "time=[0-9.]*" | cut -d= -f2)
  printf "%s %-7s %8s %8.0fms | " ${h:0:6} "$st" "$c" "$t"
  [ "$st" = "UNSATIS" -o "$st" = "SATISFI" ] && solved=$((solved+1))
  tot=$(echo "$tot + $t" | bc)
done
echo; echo "[$E] solved=$solved total_ms=$tot"
