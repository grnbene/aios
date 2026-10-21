#!/usr/bin/env sh
# Drives the chain crypto floor against two oracles that are not this build:
# published vectors, and an independent implementation signing the same inputs.
set -e
BUILD=${1:-build}
SAMPLES=${2:-10000}
HERE=$(cd "$(dirname "$0")" && pwd)

"$BUILD/aios-chain-check"

VENV="$BUILD/chain-oracle-venv"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV" >/dev/null 2>&1 || true
fi
if [ -x "$VENV/bin/python" ] && ! "$VENV/bin/python" -c "import ecdsa" >/dev/null 2>&1; then
  "$VENV/bin/pip" install --quiet ecdsa >/dev/null 2>&1 || true
fi

if [ ! -x "$VENV/bin/python" ] || ! "$VENV/bin/python" -c "import ecdsa" >/dev/null 2>&1; then
  echo "differential oracle unavailable (no python ecdsa); vectors passed, differential NOT run"
  exit 2
fi

echo "differential: $SAMPLES signatures against python-ecdsa"
"$BUILD/aios-chain-check" --emit "$SAMPLES" | "$VENV/bin/python" "$HERE/chain-oracle.py"
