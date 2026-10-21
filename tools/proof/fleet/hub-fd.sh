#!/bin/bash
BIN=$1; P=17430; K=local-test-key-not-the-fleet
for p in $(ss -ltnp 2>/dev/null | grep ":$P " | grep -o "pid=[0-9]*" | cut -d= -f2); do kill $p; done; sleep 1
rm -rf /tmp/hubfd; mkdir -p /tmp/hubfd
$BIN --root /tmp/hubfd exec "fleet hub $P $K" "sleep 120" >/tmp/hubfd.log 2>&1 & HUB=$!
sleep 1
for i in $(seq 20); do rm -rf /tmp/mfd$i; mkdir -p /tmp/mfd$i; timeout 20 $BIN --root /tmp/mfd$i exec "fleet join 127.0.0.1 $P $K" >/dev/null 2>&1; done
sleep 4
echo "  after 20 members joined and left: CLOSE-WAIT $(ss -tan "( sport = :$P )" | grep -c CLOSE-WAIT), hub fds $(ls /proc/$HUB/fd | wc -l)"
kill $HUB; rm -rf /tmp/mfd*
