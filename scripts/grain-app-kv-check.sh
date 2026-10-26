#!/usr/bin/env bash
# kv_* in a hosted app (`aios serve`): one store for the app's sessions and workers, journaled in its data directory.
# A throwaway app in a throwaway root, driven with curl: values survive an app restart and a whole host restart, an
# expired key does not come back, and a key rewritten thousands of times leaves a short journal (compaction).
#   scripts/grain-app-kv-check.sh [path/to/aios]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
aios="${1:-$here/build-static/aios}"
work="$(mktemp -d)"
host_pid=""
stop_host() { if [ -n "$host_pid" ]; then kill "$host_pid" 2>/dev/null || true; wait "$host_pid" 2>/dev/null || true; host_pid=""; fi; }
cleanup() { stop_host; rm -rf "$work"; }
trap cleanup EXIT
export AIOS_NO_ACCEL=1
host_port=$(( 30000 + RANDOM % 10000 )); app_port=$(( host_port + 1 ))
mkdir -p "$work/app"
cat > "$work/app/app.gr" <<GR
let app = {"name": "kvtest", "main": "service.gr", "kind": "web", "port": $app_port, "listen": "local", "max_steps": 0}
GR
cat > "$work/app/service.gr" <<'GR'
fn j(v) { return {"status": 200, "content_type": "application/json", "body": json_stringify(v)} }
fn incr(req) { return j({"hits": kv_incr("hits")}) }
fn settemp(req) { kv_set("temp", "short-lived", 1); kv_set("keep", {"user": "sean", "n": [1, 2]}); return j({"ok": true}) }
fn get(req) { return j({"hits": kv_get("hits"), "temp": kv_get("temp"), "keep": kv_get("keep")}) }
fn churn(req) { for i in 0..20000 { kv_set("churn", i) }; return j({"churn": kv_get("churn")}) }
ui.pane({"title": "kv", "routes": {"/incr": incr, "/settemp": settemp, "/get": get, "/churn": churn}, "view": fn () { return ui.text("kv") }})
GR
up() { for _ in $(seq 1 60); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.5; done; return 1; }
start_host() {
  "$aios" --root "$work/root" serve port=$host_port >> "$work/serve.log" 2>&1 &
  host_pid=$!
  up $host_port || { echo "the app host did not start"; cat "$work/serve.log"; exit 1; }
}
start_host
"$aios" --root "$work/root" app deploy "$work/app" > "$work/deploy.log" 2>&1 || { cat "$work/deploy.log"; exit 1; }
up $app_port || { echo "the app did not start"; cat "$work/serve.log"; exit 1; }
u="http://127.0.0.1:$app_port"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
for _ in 1 2 3 4 5; do curl -s "$u/incr" > /dev/null; done
curl -s "$u/settemp" > /dev/null
g=$(curl -s "$u/get")
check "five increments, a map and a short-lived key: $g" '[ "$g" = "{\"hits\":5,\"temp\":\"short-lived\",\"keep\":{\"user\":\"sean\",\"n\":[1,2]}}" ]'
journal=$(find "$work/root" -name kv.journal | head -1)
check "the journal is in the app's data directory: ${journal#$work/}" '[ -n "$journal" ] && [ -s "$journal" ]'

"$aios" --root "$work/root" app restart kvtest > /dev/null 2>&1
up $app_port
g=$(curl -s "$u/get")
check "after an app restart: $g" '[[ "$g" == *"\"hits\":5"* && "$g" == *"\"user\":\"sean\""* ]]'

stop_host
sleep 1.2   # the short-lived key's second runs out while nothing is running
start_host
up $app_port || { echo "the app did not come back with its host"; cat "$work/serve.log"; exit 1; }
g=$(curl -s "$u/get")
check "after a whole host restart: $g" '[[ "$g" == *"\"hits\":5"* && "$g" == *"\"user\":\"sean\""* ]]'
check "the expired key did not come back" '[[ "$g" == *"\"temp\":null"* ]]'
check "and counting goes on from 5" '[ "$(curl -s $u/incr)" = "{\"hits\":6}" ]'

c=$(curl -s "$u/churn")
size=$(stat -c %s "$journal")
check "20000 writes of one key: $c, journal $size bytes" '[ "$c" = "{\"churn\":19999}" ] && [ "$size" -lt 65536 ]'
exit $(( fails > 0 ))
