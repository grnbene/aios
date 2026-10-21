#!/usr/bin/env bash
# Drive the machine's own HTTPS and its own package manager against the REAL
# npm registry, then hand the result to the real node runtime — which is the
# only opinion that counts about whether what this machine installed is a
# package tree.
#
#   scripts/drive-npm.sh [build-dir] [work-dir]
#
# Needs outbound 443 to registry.npmjs.org. A sandbox without egress reports
# that as what it is rather than as a failure of the machine.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$here/build}
work=${2:-/tmp/aios-drive-npm}
aios=$build/aios
[ -x "$aios" ] || { echo "no command surface at $aios — build first"; exit 1; }

rm -rf "$work"; mkdir -p "$work"
root=$work/root
fail=0
step() { printf '\n=== %s\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail+1)); }
run()  { "$aios" --root "$root" exec "$@" 2>&1; }

step "can this machine reach the registry at all"
out=$(run "npm view left-pad")
echo "$out" | sed 's/^/  | /' | head -4
if echo "$out" | grep -q "published version"; then
  ok "the registry answered over the machine's own TLS"
else
  echo "  --   no answer from registry.npmjs.org; this sandbox has no egress to it"
  echo "  --   $(echo "$out" | tail -1)"
  echo; echo "NPM: SKIPPED (no route to the registry)"; exit 0
fi

step "install one package, and check every claim the report makes"
out=$(run "npm install left-pad@1.3.0 dir=$work/one")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "TLS 1" && ok "it went over TLS, and the report says which" || bad "no TLS in the report"
echo "$out" | grep -qi "trusted via" && ok "the chain reached a certificate in this machine's store" || bad "no trust anchor named"
[ -f "$work/one/node_modules/left-pad/package.json" ] && ok "the package is on disk where it belongs" || bad "nothing landed"
grep -q '"version": *"1.3.0"' "$work/one/node_modules/left-pad/package.json" 2>/dev/null \
  && ok "the version asked for is the version installed" || bad "a different version landed"

step "a dependency graph, not just one package"
out=$(run "npm install ms dir=$work/deps" )
echo "$out" | sed 's/^/  | /' | tail -3
out=$(run "npm install debug dir=$work/deps")
echo "$out" | sed 's/^/  | /' | tail -4
count=$(ls "$work/deps/node_modules" 2>/dev/null | wc -l)
[ "$count" -ge 2 ] && ok "debug brought its dependency with it ($count packages)" || bad "dependencies were not resolved"

step "REAL node's opinion of what this machine installed"
if command -v node >/dev/null; then
  got=$(cd "$work/one" && node -e "process.stdout.write(require('left-pad')('7',3,'0'))" 2>&1)
  [ "$got" = "007" ] && ok "node loaded the package and ran it: left-pad('7',3,'0') = $got" || bad "node could not use it: $got"
  got=$(cd "$work/deps" && node -e "process.stdout.write(String(require('ms')('2 days')))" 2>&1)
  [ "$got" = "172800000" ] && ok "node loaded a dependency too: ms('2 days') = $got" || bad "node could not use the dependency: $got"
else
  echo "  --   node is not installed here, so its opinion cannot be had"
fi

step "what the machine refuses, and whether it says why"
check() {
  local what="$1" want="$2"; shift 2
  local o; o=$("$aios" --root "$root" exec "$@" 2>&1)
  if echo "$o" | grep -qi "$want"; then ok "$what: $(echo "$o" | tail -1 | cut -c1-104)"
  else bad "$what: expected /$want/, got: $(echo "$o" | tail -1)"; fi
}
check "a package that does not exist"      "404"                    "npm install no-such-package-exists-here-9k dir=$work/x"
check "a version nothing satisfies"        "no published version"   "npm install left-pad@^99.0.0 dir=$work/x"
check "an entity with no network authority" "may not reach the registry" \
      "entity create outsider => o" "npm install left-pad dir=$work/x as=\$o"
check "a registry that is not there"       "cannot resolve\|refused\|io-error" \
      "npm install left-pad registry=https://registry.invalid dir=$work/x"

step "the integrity check is real: a tarball that does not hash to its claim"
# Serve a REAL package document that points at a tarball this drive has damaged,
# so the only thing wrong is the content — exactly the case the check exists for.
python3 - "$work" <<'PY' &
import http.server, json, socketserver, sys, threading, urllib.request, os
work = sys.argv[1]
doc = json.load(urllib.request.urlopen("https://registry.npmjs.org/left-pad"))
v = doc["versions"]["1.3.0"]
tar = urllib.request.urlopen(v["dist"]["tarball"]).read()
damaged = bytearray(tar); damaged[len(damaged)//2] ^= 0xff
port_holder = os.path.join(work, "port")
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.endswith(".tgz"):
            self.send_response(200); self.send_header("Content-Length", str(len(damaged))); self.end_headers()
            self.wfile.write(bytes(damaged)); return
        me = "http://127.0.0.1:%d/left-pad/-/left-pad-1.3.0.tgz" % self.server.server_address[1]
        body = json.dumps({"dist-tags": {"latest": "1.3.0"},
                           "versions": {"1.3.0": {"dist": {"tarball": me,
                                                            "integrity": v["dist"]["integrity"],
                                                            "shasum": v["dist"]["shasum"]}}}}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
with socketserver.TCPServer(("127.0.0.1", 0), H) as s:
    open(port_holder, "w").write(str(s.server_address[1]))
    s.serve_forever()
PY
serve_pid=$!
for _ in $(seq 1 60); do [ -s "$work/port" ] && break; sleep 0.5; done
port=$(cat "$work/port" 2>/dev/null)
if [ -n "$port" ]; then
  out=$(run "npm install left-pad registry=http://127.0.0.1:$port dir=$work/bad")
  echo "$out" | tail -1 | sed 's/^/  | /'
  echo "$out" | grep -qi "does not match the sha512" \
    && ok "a damaged tarball is refused by its hash, not installed" \
    || bad "a damaged tarball was not caught: $(echo "$out" | tail -1)"
  [ -d "$work/bad/node_modules/left-pad" ] && bad "it wrote the damaged package anyway" || ok "nothing from it reached the disk"
else
  echo "  --   could not stage the damaged-tarball case (no route to fetch the original)"
fi
kill $serve_pid 2>/dev/null

printf '\n'
if [ "$fail" = 0 ]; then echo "NPM: PASS"; else echo "NPM: FAIL ($fail)"; fi
exit $fail
