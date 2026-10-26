#!/usr/bin/env bash
# process_run in a hosted app (`aios serve`): refused unless the machine's operator names the app in the host's
# AIOS_APP_PROCESS_RUN - an app's own manifest settings cannot grant it. A throwaway app in a throwaway root, its
# host started three times: no setting (the manifest tries to grant itself "*"), another app named, this app named.
#   scripts/grain-app-process-check.sh [path/to/aios]
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
let app = {"name": "proctest", "main": "service.gr", "kind": "web", "port": $app_port, "listen": "local", "max_steps": 0,
  "settings": {"AIOS_APP_PROCESS_RUN": "*"}}
GR
cat > "$work/app/service.gr" <<'GR'
fn run(req) {
  let r = pcall(fn () { return process_run(["/bin/echo", "from", "the", "app"]) })
  return {"status": 200, "content_type": "application/json", "body": json_stringify(r.ok ? {"output": r.value.output} : {"error": r.error})}
}
ui.pane({"title": "proc", "routes": {"/run": run}, "view": fn () { return ui.text("proc") }})
GR
up() { for _ in $(seq 1 60); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.5; done; return 1; }
start_host() {   # $1: the host's AIOS_APP_PROCESS_RUN, or "unset"
  if [ "$1" = unset ]; then env -u AIOS_APP_PROCESS_RUN "$aios" --root "$work/root" serve port=$host_port >> "$work/serve.log" 2>&1 &
  else AIOS_APP_PROCESS_RUN="$1" "$aios" --root "$work/root" serve port=$host_port >> "$work/serve.log" 2>&1 &
  fi
  host_pid=$!
  up $host_port || { echo "the app host did not start"; cat "$work/serve.log"; exit 1; }
}
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
u="http://127.0.0.1:$app_port"

start_host unset
"$aios" --root "$work/root" app deploy "$work/app" > "$work/deploy.log" 2>&1 || { cat "$work/deploy.log"; exit 1; }
up $app_port || { echo "the app did not start"; cat "$work/serve.log"; exit 1; }
g=$(curl -s "$u/run")
check "no operator setting (the manifest's own \"*\" ignored): refused: $g" '[[ "$g" == *"not allowed here"* ]]'
stop_host

start_host "someotherapp"
up $app_port
g=$(curl -s "$u/run")
check "another app named: refused" '[[ "$g" == *"not allowed here"* ]]'
stop_host

start_host "otherapp, proctest"
up $app_port
g=$(curl -s "$u/run")
check "this app named by the operator: runs: $g" '[ "$g" = "{\"output\":\"from the app\\n\"}" ]'
exit $(( fails > 0 ))
