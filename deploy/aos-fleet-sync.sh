#!/usr/bin/env bash
# aos-fleet-sync.sh — rebuild AOS from the current source and roll it to the
# fleet, deterministically. Run from the BUILD HOST (arch): it has the fleet's
# ssh config and the source-of-truth tree. It builds the Linux static binary
# once, fans it out to the static-install machines, rebuilds arch's own member,
# restarts each service, and health-checks the relay roster.
#
#   aos-fleet-sync.sh --status    what each machine is running (build id) now
#   aos-fleet-sync.sh --dry-run    the plan, nothing executed
#   aos-fleet-sync.sh             pull main, rebuild, fan out, restart, verify
#
# SkyBlade (a macOS workstation, not reachable over ssh from here) syncs itself
# on the box with:  deploy/aos-selfsync.sh
set -uo pipefail
REPO=${AIOS_REPO:-/repos/aios}
AIOS="$REPO"
STATIC="$AIOS/build-static/aios"
HUB_STATUS_HOST=chi-il-1   # runs the observer that writes fleet-status.json

# Roster of the Linux AOS machines. Fields: name|ssh|binary path|restart command
# (restart runs over ssh on that host). arch is local (the build host itself).
# The units restarted here are in the tree: deploy/aos-fleet/aos-hub.service
# and aos-observer.service (system units running /opt/aos/aios, installed with
# `sudo deploy/aos-fleet/install.sh <user> hub|observer`) and
# deploy/aos-fleet/aos-member.service (a systemd --user unit running ~/aos/aios).
ROSTER=(
  "chi-il-1|chi-il-1|/opt/aos/aios|systemctl restart aos-hub aos-observer"
  "atl-ga-1|atl-ga-1|/opt/aos/aios|systemctl restart aos-observer"
  "devops|devops|/home/devops/aos/aios|XDG_RUNTIME_DIR=/run/user/\$(id -u) systemctl --user restart aos-member"
)

mode="${1:-apply}"

sha() { sha256sum "$1" 2>/dev/null | cut -c1-12; }
remote_sha() { ssh -o ConnectTimeout=8 "$1" "sha256sum '$2' 2>/dev/null | cut -c1-12"; }

if [ "$mode" = "--status" ]; then
  echo "== AOS fleet status =="
  echo "arch(build)   $(sha "$STATIC") (build-static)  |  member: $(systemctl is-active aos-member-roomy 2>/dev/null)"
  for row in "${ROSTER[@]}"; do IFS='|' read -r name ssh bin rc <<<"$row"
    echo "$name  bin $(remote_sha "$ssh" "$bin")"
  done
  echo "-- relay roster --"
  ssh -o ConnectTimeout=8 "$HUB_STATUS_HOST" 'python3 -c "import json;d=json.load(open(\"/opt/aos/fleet-status.json\"));print(\"reachable=%d/%d\"%(d[\"reachable\"],d[\"member_count\"]))"' 2>/dev/null || echo "(status unavailable)"
  exit 0
fi

echo "== STEP 1: pull main =="
git -C "$REPO" fetch origin main -q || { echo "FATAL: fetch failed"; exit 1; }
old=$(git -C "$REPO" rev-parse --short HEAD)
[ "$mode" = "--dry-run" ] || git -C "$REPO" merge --ff-only origin/main || { echo "FATAL: not a fast-forward"; exit 1; }
new=$(git -C "$REPO" rev-parse --short origin/main)
echo "-- $old -> $new"

echo "== STEP 2: rebuild (build + build-static) =="
if [ "$mode" = "--dry-run" ]; then
  echo "   would: cmake --build $AIOS/build -j4 ; cmake --build $AIOS/build-static -j4"
else
  cmake --build "$AIOS/build" -j4 >/tmp/aos-build.log 2>&1 && cmake --build "$AIOS/build-static" -j4 >>/tmp/aos-build.log 2>&1 || { echo "FATAL: build failed (see /tmp/aos-build.log)"; tail -5 /tmp/aos-build.log; exit 1; }
  echo "-- built static $(sha "$STATIC")"
fi

echo "== STEP 3: arch member =="
if [ "$mode" = "--dry-run" ]; then echo "   would: sudo -n systemctl restart aos-member-roomy"
else sudo -n systemctl restart aos-member-roomy && echo "-- arch member restarted"; fi

echo "== STEP 4: fan out static + restart =="
for row in "${ROSTER[@]}"; do IFS='|' read -r name ssh bin rc <<<"$row"
  if [ "$mode" = "--dry-run" ]; then echo "   would: scp static -> $ssh:$bin ; ssh $ssh '$rc'"; continue; fi
  echo "-- $name"
  scp -o ConnectTimeout=10 "$STATIC" "$ssh:$bin.new" && ssh "$ssh" "mv '$bin.new' '$bin' && chmod +x '$bin' && $rc" && echo "   $name updated + restarted" || echo "   $name FAILED"
done

echo "== STEP 5: health-check relay roster =="
[ "$mode" = "--dry-run" ] && { echo "   would: read fleet-status.json on $HUB_STATUS_HOST"; exit 0; }
sleep 6
ssh -o ConnectTimeout=8 "$HUB_STATUS_HOST" 'python3 -c "import json;d=json.load(open(\"/opt/aos/fleet-status.json\"));print(\"reachable=%d/%d\"%(d[\"reachable\"],d[\"member_count\"]))"'
echo "== done ($new) — remember: SkyBlade self-syncs with deploy/aos-selfsync.sh =="
