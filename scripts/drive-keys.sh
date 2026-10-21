#!/bin/sh
# The keyboard, driven through the real pane: a form filled in and submitted
# with Enter, and the four editing keys a person's hands already know.
#
# This cannot be a layout golden. Enter-to-submit is a NAVIGATION -- the proof
# is the url the pane ends up at, and the only way to see it is to press the
# keys and look. A local server echoes the query back so the check is exact:
# what arrived is what was typed.
#
#   scripts/drive-keys.sh <build-dir>
set -eu
BUILD=${1:-build}
DRIVE="$BUILD/aios-web-pane-drive"
[ -x "$DRIVE" ] || { echo "drive.keys: no $DRIVE" >&2; exit 1; }

WORK=$(mktemp -d)
PORT=8949
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

cat > "$WORK/k.html" <<'HTML'
<html><head><title>Keys</title><style>body{background:#fff;color:#111;font:15px sans-serif}
input{font:15px sans-serif}</style></head><body>
<form action="/echo" method="get"><input id="q" name="q" size="30" value="">
<input type="submit" value="Go"></form></body></html>
HTML

python3 - "$WORK" "$PORT" <<'PY' &
import http.server, socketserver, urllib.parse, sys, os
root, port = sys.argv[1], int(sys.argv[2])
os.chdir(root)
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/echo':
            q = urllib.parse.parse_qs(u.query).get('q', [''])[0]
            b = ('<html><head><title>' + q + '</title></head><body><p>ok</p></body></html>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        return super().do_GET()
    def log_message(self, *a): pass
socketserver.TCPServer.allow_reuse_address = True
socketserver.TCPServer(('127.0.0.1', port), H).serve_forever()
PY
SRV=$!
sleep 1

fail=0
n=0
check() {
  want=$1; keys=$2; what=$3
  n=$((n + 1))
  got=$(AIOS_PANE_SIZE=1400x800 AIOS_CLICK_ID=q AIOS_KEYS="$keys" \
        "$DRIVE" "$WORK/root$n" "$WORK/out$n" "http://127.0.0.1:$PORT/k.html" 2>/dev/null \
        | sed -n 's/^ *now at *: .*[?&]q=\(.*\)$/\1/p' | tail -1)
  if [ "$got" = "$want" ]; then
    echo "  ok   $what"
  else
    echo "  FAIL $what: q=$got, wanted q=$want" >&2
    fail=$((fail + 1))
  fi
}

# Enter in a field inside a form submits it. Everything else is editing, and
# every one of them is checked by what the server was actually asked for.
check "ring+zero"   '+ring zero,Enter'                          "Enter submits the form"
check "world"       '+hello,CtrlA,+world,Enter'                 "Ctrl+A then typing replaces"
check "z"           '+abcdef,CtrlA,Backspace,+z,Enter'          "Ctrl+A then Backspace empties"
check "xalpha"      '+alpha,CtrlA,CtrlC,CtrlA,+x,CtrlV,Enter'   "Ctrl+C then Ctrl+V"
check "cutmecutme"  '+cutme,CtrlX,CtrlV,CtrlV,Enter'            "Ctrl+X cuts to the clipboard"
check "clipme"      '+clipme,CtrlA,CtrlC,CtrlL,CtrlA,+http://127.0.0.1:8949/echo?q=,CtrlV,Enter' \
                    "the clipboard crosses to the address bar"

if [ "$fail" -ne 0 ]; then
  echo "drive.keys: $fail of $n checks failed" >&2
  exit 1
fi
echo "drive.keys: $n keyboard checks pass"
