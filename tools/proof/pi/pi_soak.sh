#!/bin/bash
# Wait for the Pi, then reset it N times; each boot must come back on its own.
N=${1:-10}
log=/tmp/pi-soak.log; : > $log
n0=$(wc -l < /tmp/pi-crumbs.log)
until tail -n +$((n0+1)) /tmp/pi-crumbs.log | grep -q "hb loops"; do sleep 3; done
echo "$(date +%T) initial boot up" >> $log
sleep 10
for i in $(seq 1 $N); do
  t0=$(date +%s); since=$(date '+%Y-%m-%d %H:%M:%S')
  n0=$(wc -l < /tmp/pi-crumbs.log)
  (echo "reset"; sleep 2) | timeout 4 nc -u -w2 ${AIOS_PI:?set AIOS_PI to the board ip} 42424 >/dev/null
  ok=0
  for k in $(seq 1 60); do
    sleep 3
    if tail -n +$((n0+1)) /tmp/pi-crumbs.log | grep -q "hb loops"; then ok=1; break; fi
  done
  offers=$(journalctl -u aios-netboot --since "$since" --no-pager | grep -c "DHCPOFFER(br0) ${AIOS_PI:?set AIOS_PI to the board ip}")
  if [ $ok = 1 ]; then echo "$(date +%T) reset $i: UP in $(( $(date +%s) - t0 ))s, our offers=$offers" >> $log
  else echo "$(date +%T) reset $i: DID NOT COME BACK (180s), our offers=$offers" >> $log; exit 1; fi
  sleep 8
done
echo "$(date +%T) SOAK PASSED: $N/$N resets came back" >> $log
