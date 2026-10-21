#!/bin/bash
# Reset the Pi and wait until it has genuinely rebooted (uptime small again),
# through the fleet's authenticated node link - not the raw UDP console.
#
# The raw console defaults to LOCKED each boot (metal/arm64/net.cpp's
# g_console_debug) - `reset` sent there is silently refused, and the OLD
# version of this script's "pi up" check accepted ANY reply, including that
# refusal, so it reported success without the board ever actually resetting.
# Confirmed on real hardware: the board sat at 47+ minutes uptime through
# several "pi up after 22s" reports before this was caught. `fleet run
# <peer> reset` bypasses the lock (fleet_run_command doesn't refuse `reset`
# the way it refuses `desktop`/`gui`/`dash`/`fleet` - it dispatches it, the
# reply never makes it back before PSCI fires, but the reset itself still
# happens), and polling uptime (not just "did anything reply") is what
# actually proves a fresh boot rather than a live-but-stale one.
cd /repos/aios || exit 1
PEER=ce4d

python3 tools/grain_remote.py --fleet shell "reset" >/dev/null 2>&1

for i in $(seq 1 60); do
  sleep 2
  out=$(timeout 8 python3 tools/grain_remote.py --fleet shell "info" 2>/dev/null)
  up=$(echo "$out" | grep -oP 'uptime\s*:\s*\K[0-9]+')
  if [ -n "$up" ] && [ "$up" -lt 20000 ]; then
    echo "pi up after $((i*2))s (uptime ${up}ms)"
    exit 0
  fi
done
echo "pi NOT up (still on old uptime, or unreachable)"
exit 1
