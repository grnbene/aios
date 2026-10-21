#!/usr/bin/env bash
# clicks.sh PORT: order N invoiced by 3 clicks at once, then 3 more staggered (each after the one before finished)
P=$1; bad=0
for o in 3 4 5 6 7 8; do for i in 1 2 3; do php drive.php $P $o TAG=c$i & done > /tmp/clk.$o 2>&1; wait; done
for o in 9 10 11; do for i in 1 2 3; do php drive.php $P $o TAG=s$i; done > /tmp/clk.$o 2>&1; done
for o in 3 4 5 6 7 8 9 10 11; do n=$(${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -d summitholding -e "select count(*) from debtortrans where type=10 and order_=$o" | grep -E '^\| [0-9]' | tr -d '| '); [ "$n" = 1 ] || bad=$((bad+1)); printf "order %s: %s invoice(s)  " $o $n; done; echo; echo "orders not invoiced exactly once: $bad"
