#!/usr/bin/env bash
# aos-selfsync.sh — pull, rebuild, and restart the AOS kernel on THIS machine.
# For a source-build host (e.g. a macOS workstation) that the ssh-based fleet
# sync can't reach. Run it on the box after AOS updates are pushed to main. The
# kernel re-joins the fleet on its own from AIOS_FLEET_HUB after the restart.
#   deploy/aos-selfsync.sh [--dry-run]   (AOS_SYNC_REMOTE / AOS_SYNC_BRANCH: default origin main)
set -uo pipefail
AIOS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the aios source (holds scripts/aos.sh)
# The git checkout that holds it: the aios tree itself when it is a standalone
# clone, or the monorepo around it when aios is a subdirectory.
TREE="$(git -C "$AIOS" rev-parse --show-toplevel 2>/dev/null)" || { echo "FATAL: $AIOS is not inside a git checkout"; exit 1; }
REMOTE=${AOS_SYNC_REMOTE:-origin}; BRANCH=${AOS_SYNC_BRANCH:-main}
echo "== pull $REMOTE/$BRANCH into $TREE =="
if [ "${1:-}" = "--dry-run" ]; then
  echo "   would: git -C $TREE fetch $REMOTE $BRANCH && git -C $TREE merge --ff-only $REMOTE/$BRANCH"
  echo "   would: $AIOS/scripts/aos.sh build ; $AIOS/scripts/aos.sh restart"
  exit 0
fi
git -C "$TREE" fetch "$REMOTE" "$BRANCH" -q && git -C "$TREE" merge --ff-only "$REMOTE/$BRANCH" || { echo "FATAL: not a fast-forward — resolve manually"; exit 1; }
echo "-- now at $(git -C "$TREE" rev-parse --short HEAD)"
echo "== rebuild =="; bash "$AIOS/scripts/aos.sh" build || { echo "FATAL: build failed"; exit 1; }
echo "== restart (auto-rejoins fleet from env) =="; bash "$AIOS/scripts/aos.sh" restart
sleep 4; bash "$AIOS/scripts/aos.sh" status | head -6
