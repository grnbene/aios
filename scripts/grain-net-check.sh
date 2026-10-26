#!/usr/bin/env bash
# Runs the grain scripts that need a local HTTP server (tests/grain/test_http_gzip.gr) against
# tests/grain/net/gzip_server.py on a free loopback port; the server is stopped afterwards.
#   scripts/grain-net-check.sh [path/to/grain]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
grain="${1:-$here/build-grain/grain}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null && wait "$server_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
python3 -I "$here/tests/grain/net/gzip_server.py" "$work/port" &
server_pid=$!
for _ in $(seq 1 50); do [ -s "$work/port" ] && break; sleep 0.1; done
[ -s "$work/port" ] || { echo "the test HTTP server did not start"; exit 1; }
export GRAIN_TEST_HTTP_PORT="$(cat "$work/port")"
rc=0
for t in "$here"/tests/grain/test_http_gzip.gr; do
  echo "== $(basename "$t")"
  "$grain" "$t" || rc=1
done
exit $rc
