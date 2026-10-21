#!/bin/bash
# A member that heart-beats but stops reading (the Pi on its one loop) must
# not take the hub down for every other member.
BIN=$1; P=17420; K=local-test-key-not-the-fleet
for p in $(pgrep -f "fleet (hub|join 127.0.0.1) $P"); do kill -9 $p; done 2>/dev/null; sleep 1
rm -rf /tmp/hubt /tmp/m[ABDE] /tmp/stall-reads; mkdir -p /tmp/hubt /tmp/mA /tmp/mB /tmp/mD /tmp/mE
$BIN --root /tmp/hubt exec "fleet hub $P $K" "sleep 600" >/tmp/hubt.log 2>&1 & HUB=$!
sleep 1
B_ID=$($BIN --root /tmp/mB exec whoami 2>&1 | grep -o "kernel [0-9a-f]*" | cut -d' ' -f2)
LD_PRELOAD=/tmp/smallrcv.so $BIN --root /tmp/mB exec "fleet join 127.0.0.1 $P $K" "sleep 600" >/tmp/mB.log 2>&1 & BP=$!
sleep 2
sed "s/env_b/\"$B_ID\"/" /tmp/flood.gr > /tmp/flood-run.gr
$BIN --root /tmp/mA exec "fleet join 127.0.0.1 $P $K" "sleep 2" "grain run /tmp/flood-run.gr" >/tmp/mA.log 2>&1 & AP=$!
until grep -q "stop B now" /tmp/mA.log; do sleep 0.2; done; touch /tmp/stall-reads; T0=$(date +%s)
sleep 6; ss -tn | grep 17420 > /tmp/hub-ss.txt
echo "t+$(( $(date +%s)-T0 ))s  hub->B Send-Q: $(ss -tn "( sport = :$P )" | awk 'NR>1{print $3}' | sort -n | tail -1) bytes"
$BIN --root /tmp/mD exec "fleet join 127.0.0.1 $P $K" "sleep 30" >/tmp/mD.log 2>&1 & DP=$!
sleep 3
echo "t+$(( $(date +%s)-T0 ))s  E tries to attach:"
timeout 20 $BIN --root /tmp/mE exec "fleet join 127.0.0.1 $P $K" 2>&1 | tail -1 | sed 's/^/    /'
echo "t+$(( $(date +%s)-T0 ))s  D: $(head -1 /tmp/mD.log)"
rm -f /tmp/stall-reads; kill -9 $BP $AP $DP $HUB 2>/dev/null
