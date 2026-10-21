#!/usr/bin/env bash
# Drive the machine's Go module client against the REAL proxy and the REAL
# checksum database, and then attack it: a proxy that serves a different module
# under the right name, and a checksum database that signs a real tree but
# reports a record that is not in it. Both must be refused, by name.
#
#   scripts/drive-go.sh [build-dir] [work-dir]
#
# Needs outbound 443 to proxy.golang.org and sum.golang.org. A sandbox without
# egress reports that as what it is rather than as a failure of the machine.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$here/build}
work=${2:-/tmp/aios-drive-go}
aios=$build/aios
[ -x "$aios" ] || { echo "no command surface at $aios — build first"; exit 1; }

rm -rf "$work"; mkdir -p "$work"
root=$work/root
fail=0
step() { printf '\n=== %s\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail+1)); }
run()  { "$aios" --root "$root" exec "$@" 2>&1; }

step "can this machine reach the module proxy at all"
out=$(run "go list rsc.io/quote")
echo "$out" | sed 's/^/  | /' | head -2
if echo "$out" | grep -q "version(s)"; then
  ok "proxy.golang.org answered over the machine's own TLS"
else
  echo "  --   no answer from proxy.golang.org; this sandbox has no egress to it"
  echo "  --   $(echo "$out" | tail -1)"
  echo; echo "GO: SKIPPED (no route to the proxy)"; exit 0
fi

step "a module, its requirements, and the signed tree they were proved into"
out=$(run "go get rsc.io/quote@v1.5.2 dir=$work/m")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "TLS 1" && ok "it went over TLS, and the report says which" || bad "no TLS in the report"
echo "$out" | grep -q "signed by sum.golang.org" && ok "the tree head carries sum.golang.org's own signature" \
  || bad "the checksum database was not consulted"
tiles=$(echo "$out" | sed -n 's/.*(\([0-9]*\) log tiles read.*/\1/p')
[ -n "$tiles" ] && [ "$tiles" -gt 0 ] && ok "the inclusion proof read $tiles log tiles to reach the signed root" \
  || bad "no inclusion proof was performed"
for m in rsc.io/quote rsc.io/sampler golang.org/x/text; do
  [ -d "$work/m/vendor/$m" ] && ok "$m is on disk" || bad "$m did not land"
done
[ -f "$work/m/vendor/rsc.io/quote/quote.go" ] && ok "the source itself is there, not just a directory" \
  || bad "the module's own source is missing"

step "the hashes, against the database's own published record — an independent opinion"
if command -v curl >/dev/null; then
  agree=0; disagree=0
  while read -r mod ver hash; do
    case "$ver" in */go.mod) continue;; esac
    want=$(curl -sf "https://sum.golang.org/lookup/$mod@$ver" | sed -n "s|^$mod $ver \(h1:.*\)$|\1|p" | head -1)
    if [ "$want" = "$hash" ]; then agree=$((agree+1)); else disagree=$((disagree+1)); echo "  --   $mod $ver: $hash vs $want"; fi
  done < "$work/m/go.sum"
  [ "$disagree" = 0 ] && [ "$agree" -ge 3 ] \
    && ok "every h1 this machine computed equals the one sum.golang.org publishes ($agree modules)" \
    || bad "the machine's hashes disagree with the database ($disagree of $((agree+disagree)))"
else
  echo "  --   curl is not here, so the independent opinion cannot be had"
fi

step "go.sum is written the way a Go build reads it"
lines=$(wc -l < "$work/m/go.sum")
[ "$lines" = 6 ] && ok "six lines: a zip hash and a go.mod hash for each of the three modules" \
  || bad "go.sum has $lines lines"
grep -q "^rsc.io/quote v1.5.2 h1:w5fcysjrx7yqtD/aO+QwRjYZOKnaM9Uh2b40tElTs3Y=$" "$work/m/go.sum" \
  && ok "and the line for rsc.io/quote is byte for byte what go writes" || bad "the go.sum line is not what go writes"

step "re-hashing what is on disk, and noticing a single altered byte"
out=$(run "go mod verify dir=$work/m")
echo "$out" | tail -2 | sed 's/^/  | /'
echo "$out" | grep -q "still hash to what go.sum recorded" && ok "the vendor tree still hashes to its record" \
  || bad "verify did not pass on a tree this machine just wrote"
printf '\n// not what was downloaded\n' >> "$work/m/vendor/rsc.io/quote/quote.go"
out=$(run "go mod verify dir=$work/m")
echo "$out" | tail -1 | sed 's/^/  | /'
echo "$out" | grep -q "altered since it was downloaded" && ok "one appended comment is caught, and the module is named" \
  || bad "an altered tree passed verification"

step "a proxy that serves a DIFFERENT module under the right name"
python3 - "$work" <<'PY' &
import http.server, socketserver, sys, os, io, zipfile, urllib.request
work = sys.argv[1]
UP = "https://proxy.golang.org"
def up(path):
    return urllib.request.urlopen(UP + path, timeout=60).read()
zbytes = up("/rsc.io/quote/@v/v1.5.2.zip")
zin = zipfile.ZipFile(io.BytesIO(zbytes))
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zout:
    for i in zin.infolist():
        data = zin.read(i.filename)
        if i.filename.endswith("quote.go"):
            data = data + b"\n// a line the author never wrote\n"
        zout.writestr(i.filename, data)
tampered = buf.getvalue()
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = tampered if self.path.endswith(".zip") else up(self.path)
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
with socketserver.TCPServer(("127.0.0.1", 0), H) as s:
    s.allow_reuse_address = True
    open(os.path.join(work, "proxy-port"), "w").write(str(s.server_address[1]))
    s.serve_forever()
PY
proxy_pid=$!
for _ in $(seq 1 60); do [ -s "$work/proxy-port" ] && break; sleep 0.5; done
pport=$(cat "$work/proxy-port" 2>/dev/null)
if [ -n "$pport" ]; then
  out=$(run "go get rsc.io/quote@v1.5.2 deps=0 dir=$work/lied proxy=http://127.0.0.1:$pport")
  echo "$out" | tail -1 | sed 's/^/  | /'
  echo "$out" | grep -q "not the same module" \
    && ok "the substitution is refused: its dirhash is not the recorded one" \
    || bad "a tampered module was accepted: $(echo "$out" | tail -1)"
  [ -d "$work/lied/vendor/rsc.io/quote" ] && bad "it wrote the tampered module anyway" \
    || ok "nothing from it reached the disk"
else
  echo "  --   could not stage the lying proxy"
fi
kill $proxy_pid 2>/dev/null

step "a checksum database that signs a real tree but reports a record not in it"
python3 - "$work" <<'PY' &
import http.server, socketserver, sys, os, urllib.request
work = sys.argv[1]
UP = "https://sum.golang.org"
real = urllib.request.urlopen(UP + "/lookup/rsc.io/quote@v1.5.2", timeout=60).read().decode()
# The tree head and its signature are left exactly as they are. Only the record
# is rewritten — which is precisely what an inclusion proof exists to catch.
forged = real.replace("h1:w5fcysjrx7yqtD/aO+QwRjYZOKnaM9Uh2b40tElTs3Y=",
                      "h1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = forged.encode() if self.path.startswith("/lookup/") else urllib.request.urlopen(UP + self.path, timeout=60).read()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
with socketserver.TCPServer(("127.0.0.1", 0), H) as s:
    open(os.path.join(work, "sumdb-port"), "w").write(str(s.server_address[1]))
    s.serve_forever()
PY
sum_pid=$!
for _ in $(seq 1 60); do [ -s "$work/sumdb-port" ] && break; sleep 0.5; done
sport=$(cat "$work/sumdb-port" 2>/dev/null)
if [ -n "$sport" ]; then
  out=$(run "go get rsc.io/quote@v1.5.2 deps=0 dir=$work/split sumdb=http://127.0.0.1:$sport")
  echo "$out" | tail -1 | sed 's/^/  | /'
  echo "$out" | grep -q "split view" \
    && ok "the forged record does not prove into the signed tree, and the machine says so" \
    || bad "a forged record was believed: $(echo "$out" | tail -1)"
  [ -d "$work/split/vendor/rsc.io/quote" ] && bad "it wrote the module anyway" || ok "nothing reached the disk"
else
  echo "  --   could not stage the lying checksum database"
fi
kill $sum_pid 2>/dev/null

step "what the machine refuses, and whether it says why"
check() {
  local what="$1" want="$2"; shift 2
  local o; o=$("$aios" --root "$root" exec "$@" 2>&1)
  if echo "$o" | grep -qi "$want"; then ok "$what: $(echo "$o" | tail -1 | cut -c1-112)"
  else bad "$what: expected /$want/, got: $(echo "$o" | tail -1)"; fi
}
check "a module that does not exist"      "404\|410\|not-found"   "go get github.com/nosuch/moduleexists9k deps=0 dir=$work/x"
check "a version nothing answers to"      "no version answers\|404\|410" "go get rsc.io/quote@v99 deps=0 dir=$work/x"
check "a well-formed key that did not sign it" "did not sign this\|different key" \
      "go get rsc.io/quote@v1.5.2 deps=0 dir=$work/x key=sum.golang.org+70b26aea+AQABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f"
check "a verifier key with a wrong hash"  "does not match the key" \
      "go get rsc.io/quote@v1.5.2 deps=0 dir=$work/x key=sum.golang.org+00000000+Ac4zctda0e5eza+HJyk9SxEdh+s3Ux18htTTAD8OuAn8"
check "an entity with no network authority" "may not reach the module proxy" \
      "entity create outsider => o" "go get rsc.io/quote deps=0 dir=$work/x as=\$o"
check "verify where nothing was recorded"  "nothing here records"  "go mod verify dir=$work/empty"

printf '\n'
if [ "$fail" = 0 ]; then echo "GO: PASS"; else echo "GO: FAIL ($fail)"; fi
exit $fail
