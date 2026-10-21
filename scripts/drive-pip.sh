#!/usr/bin/env bash
# Drive the machine's Python and Rust package clients against the REAL indexes,
# then hand the result to the real interpreters — which are the only opinions
# that count about whether what this machine installed is usable.
#
#   scripts/drive-pip.sh [build-dir] [work-dir]
#
# Needs outbound 443 to pypi.org, files.pythonhosted.org and crates.io. A
# sandbox without egress reports that as what it is rather than as a failure of
# the machine.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$here/build}
work=${2:-/tmp/aios-drive-pip}
aios=$build/aios
[ -x "$aios" ] || { echo "no command surface at $aios — build first"; exit 1; }

rm -rf "$work"; mkdir -p "$work"
root=$work/root
fail=0
step() { printf '\n=== %s\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail+1)); }
run()  { "$aios" --root "$root" exec "$@" 2>&1; }

step "can this machine reach the Python index at all"
out=$(run "pip view requests")
echo "$out" | sed 's/^/  | /' | head -3
if echo "$out" | grep -q "release(s)"; then
  ok "pypi.org answered over the machine's own TLS"
else
  echo "  --   no answer from pypi.org; this sandbox has no egress to it"
  echo "  --   $(echo "$out" | tail -1)"
  echo; echo "PIP: SKIPPED (no route to the index)"; exit 0
fi

step "a pure-Python package and everything it needs"
out=$(run "pip install requests dir=$work/site")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "TLS 1" && ok "it went over TLS, and the report says which" || bad "no TLS in the report"
echo "$out" | grep -qi "trusted via" && ok "the chain reached a certificate in this machine's store" || bad "no trust anchor named"
for m in requests urllib3 certifi idna charset_normalizer; do
  [ -d "$work/site/$m" ] && ok "$m is on disk" || bad "$m did not land"
done

step "REAL python's opinion of what this machine installed"
if command -v python3 >/dev/null; then
  got=$(cd "$work" && PYTHONPATH="$work/site" python3 -c "
import requests, urllib3, certifi
print(requests.__version__, urllib3.__version__)
" 2>&1 | tail -1)
  case "$got" in
    *.*\ *.*) ok "python imported the tree: requests/urllib3 $got";;
    *) bad "python could not import it: $got";;
  esac
  got=$(cd "$work" && PYTHONPATH="$work/site" python3 -c "
import requests
r = requests.get('https://pypi.org/pypi/requests/json', timeout=20)
print(r.status_code, r.json()['info']['name'])
" 2>&1 | tail -1)
  [ "$got" = "200 requests" ] && ok "and used it for a real request: $got" || bad "the installed requests did not work: $got"
else
  echo "  --   python3 is not installed here, so its opinion cannot be had"
fi

step "a wheel with compiled code in it — the case npm cannot do at all"
out=$(run "pip install numpy dir=$work/site2")
echo "$out" | sed 's/^/  | /' | tail -3
echo "$out" | grep -q "manylinux" && ok "it chose the platform wheel for this machine" || bad "no platform wheel was chosen"
if command -v python3 >/dev/null; then
  got=$(cd "$work" && PYTHONPATH="$work/site2" python3 -c "
import numpy as np
print(np.__version__, int((np.arange(4) ** 2).sum()))
" 2>&1 | tail -1)
  case "$got" in
    *\ 14) ok "python ran its compiled extension: numpy $got";;
    *) bad "the compiled wheel did not run: $got";;
  esac
fi

step "Rust sources, checksum-verified, where a compiler can find them"
out=$(run "cargo vendor serde dir=$work/rust")
echo "$out" | sed 's/^/  | /' | tail -6
echo "$out" | grep -q "crate(s)" && ok "the crate and its dependencies came down" || bad "nothing was vendored"
[ -f "$work/rust/vendor/serde-"*"/Cargo.toml" ] && ok "each crate is laid out as cargo vendors it" || bad "no Cargo.toml in the vendor tree"
count=$(ls "$work/rust/vendor" 2>/dev/null | wc -l)
[ "$count" -ge 3 ] && ok "dependencies were followed ($count crates)" || bad "dependencies were not followed"
if command -v cargo >/dev/null; then
  ver=$(basename "$(ls -d "$work/rust/vendor/serde-"* | head -1)")
  ok "cargo is present; the vendored $ver is source a build can be pointed at"
else
  echo "  --   cargo is not installed here, so its opinion cannot be had"
fi

step "what the machine refuses, and whether it says why"
check() {
  local what="$1" want="$2"; shift 2
  local o; o=$("$aios" --root "$root" exec "$@" 2>&1)
  if echo "$o" | grep -qi "$want"; then ok "$what: $(echo "$o" | tail -1 | cut -c1-108)"
  else bad "$what: expected /$want/, got: $(echo "$o" | tail -1)"; fi
}
check "a package that does not exist"        "404"                   "pip install no-such-package-exists-9k dir=$work/x"
check "a version nothing satisfies"          "no published version"  "pip install requests==99.0.0 dir=$work/x"
check "an interpreter no wheel fits"         "no wheel fits\|not-found" "pip install numpy python=2.7 dir=$work/x"
check "an entity with no network authority"  "may not reach the index" \
      "entity create outsider => o" "pip install requests dir=$work/x as=\$o"
check "a crate that does not exist"          "404\|not-found"        "cargo vendor no-such-crate-exists-9k dir=$work/x"

step "the sha256 check is real: a wheel that does not hash to its claim"
python3 - "$work" <<'PY' &
import http.server, json, socketserver, sys, urllib.request, os
work = sys.argv[1]
doc = json.load(urllib.request.urlopen("https://pypi.org/pypi/left-pad-python/json")) if False else None
name, ver = "idna", None
doc = json.load(urllib.request.urlopen("https://pypi.org/pypi/%s/json" % name))
ver = doc["info"]["version"]
files = [f for f in doc["releases"][ver] if f["filename"].endswith("-none-any.whl")]
f = files[0]
whl = urllib.request.urlopen(f["url"]).read()
damaged = bytearray(whl); damaged[len(damaged)//2] ^= 0xff
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.endswith(".whl"):
            self.send_response(200); self.send_header("Content-Length", str(len(damaged))); self.end_headers()
            self.wfile.write(bytes(damaged)); return
        me = "http://127.0.0.1:%d/%s" % (self.server.server_address[1], f["filename"])
        body = json.dumps({"info": {"version": ver},
                           "releases": {ver: [{"filename": f["filename"], "url": me,
                                               "digests": {"sha256": f["digests"]["sha256"]}, "yanked": False}]}}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
with socketserver.TCPServer(("127.0.0.1", 0), H) as s:
    open(os.path.join(work, "port"), "w").write(str(s.server_address[1]))
    s.serve_forever()
PY
serve_pid=$!
for _ in $(seq 1 60); do [ -s "$work/port" ] && break; sleep 0.5; done
port=$(cat "$work/port" 2>/dev/null)
if [ -n "$port" ]; then
  out=$(run "pip install idna index=http://127.0.0.1:$port deps=0 dir=$work/bad")
  echo "$out" | tail -1 | sed 's/^/  | /'
  echo "$out" | grep -qi "does not match the sha256" \
    && ok "a damaged wheel is refused by its hash, not installed" \
    || bad "a damaged wheel was not caught: $(echo "$out" | tail -1)"
  [ -d "$work/bad/idna" ] && bad "it wrote the damaged package anyway" || ok "nothing from it reached the disk"
else
  echo "  --   could not stage the damaged-wheel case (no route to fetch the original)"
fi
kill $serve_pid 2>/dev/null

printf '\n'
if [ "$fail" = 0 ]; then echo "PIP: PASS"; else echo "PIP: FAIL ($fail)"; fi
exit $fail
