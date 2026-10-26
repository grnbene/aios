#!/usr/bin/env bash
# The app host (`aios serve`) gzips a response for a client that accepts it: a throwaway app in a throwaway root,
# checked with curl - gzip only when asked (not for gzip;q=0), only for text-like types of 1 KB or more, Vary set,
# and the decoded body the same as the plain one. The host and its root are removed afterwards.
#   scripts/grain-app-gzip-check.sh [path/to/aios]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
aios="${1:-$here/build-static/aios}"
work="$(mktemp -d)"
host_pid=""
cleanup() { [ -n "$host_pid" ] && kill "$host_pid" 2>/dev/null && wait "$host_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
export AIOS_NO_ACCEL=1
host_port=$(( 30000 + RANDOM % 10000 )); app_port=$(( host_port + 1 ))
mkdir -p "$work/app"
cat > "$work/app/app.gr" <<GR
let app = {"name": "gztest", "main": "service.gr", "kind": "web", "port": $app_port, "listen": "local", "max_steps": 0}
GR
cat > "$work/app/service.gr" <<'GR'
fn big(req) { return {"status": 200, "content_type": "application/json; charset=utf-8", "body": json_stringify({"items": range(0, 3000)})} }
fn small(req) { return {"status": 200, "content_type": "application/json", "body": "{\"a\":1}"} }
fn png(req) { return {"status": 200, "content_type": "image/png", "body": repeat("x", 5000)} }
ui.pane({"title": "gz", "routes": {"/big": big, "/small": small, "/png": png}, "view": fn () { return ui.text("gz") }})
GR
"$aios" --root "$work/root" serve port=$host_port > "$work/serve.log" 2>&1 &
host_pid=$!
up() { for _ in $(seq 1 60); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.5; done; return 1; }
up $host_port || { echo "the app host did not start"; cat "$work/serve.log"; exit 1; }
"$aios" --root "$work/root" app deploy "$work/app" > "$work/deploy.log" 2>&1 || { cat "$work/deploy.log"; exit 1; }
up $app_port || { echo "the app did not start"; cat "$work/serve.log"; exit 1; }
u="http://127.0.0.1:$app_port"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
hdr() { curl -s -D - -o /dev/null "${@:2}" "$u$1" | tr -d '\r'; }
h=$(hdr /big -H 'Accept-Encoding: gzip')
check "big JSON, gzip accepted: gzip" '[[ "$h" == *"Content-Encoding: gzip"* ]]'
check "Vary: Accept-Encoding" '[[ "$h" == *"Vary: Accept-Encoding"* ]]'
check "the gzip body decodes to the plain one" '[ "$(curl -s -H "Accept-Encoding: gzip" $u/big | gzip -dc | md5sum)" = "$(curl -s $u/big | md5sum)" ]'
h=$(hdr /big)
check "no Accept-Encoding: plain" '[[ "$h" != *"Content-Encoding"* ]]'
h=$(hdr /big -H 'Accept-Encoding: gzip;q=0, identity')
check "gzip;q=0: plain" '[[ "$h" != *"Content-Encoding"* ]]'
h=$(hdr /small -H 'Accept-Encoding: gzip')
check "under 1 KB: plain" '[[ "$h" != *"Content-Encoding"* ]]'
h=$(hdr /png -H 'Accept-Encoding: gzip')
check "an image: plain" '[[ "$h" != *"Content-Encoding"* ]]'
exit $(( fails > 0 ))
