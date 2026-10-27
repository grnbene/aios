#!/usr/bin/env bash
# A ui.pane given to another fleet member (ui.pane "machine"): on a private fleet (a hub and two app hosts on
# loopback), member B's copy gives its pane to member A. Through B: the page is A's render with A's session cookie,
# an action runs on A, B's open page stream gets A's next frame, a route answers on A; a pane given to B itself is
# answered on B.
#
#   pane-machine-check.sh <aios binary>
set -u
BIN=${1:?usage: pane-machine-check.sh <aios binary>}
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
fail() { echo "FAIL $*"; exit 1; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

HUB=$(free_port); CA=$(free_port); CB=$(free_port); PA=$(free_port); PB=$(free_port)
umask 077
head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/key"
"$BIN" --root "$W/hub" exec "fleet hub $HUB @$W/key bind=127.0.0.1" "sleep 600" > "$W/hub.log" 2>&1 < /dev/null & PIDS+=($!)
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$HUB) 2>/dev/null && break; sleep 0.1; done
for h in A B; do
  cp=$([ $h = A ] && echo $CA || echo $CB); port=$([ $h = A ] && echo $PA || echo $PB)
  AIOS_FLEET_HUB=127.0.0.1:$HUB AIOS_FLEET_KEY=$(cat "$W/key") "$BIN" --root "$W/root$h" exec "serve port=$cp" > "$W/serve$h.log" 2>&1 < /dev/null & PIDS+=($!)
  mkdir -p "$W/app$h"
  cat > "$W/app$h/app.gr" <<EOF
let app = {"name": "panetest", "main": "service.gr", "kind": "web", "port": $port, "listen": "local", "settings": {"MARK": "", "PANE_ON": ""}}
EOF
  cat > "$W/app$h/service.gr" <<'EOF'
let n = 0
ui.pane({"title": "Pane test", "machine": env("PANE_ON", ""),
  "view": fn() { return el("div", el("p", {"id": "who"}, "rendered on " + env("MARK", "?")), el("p", {"id": "n"}, "n=" + str(n)),
                         el("button", {"on_click": fn() { n += 1 }}, "Add")) },
  "routes": {"/where": fn(req) { return {"status": 200, "content_type": "text/plain", "body": "route on " + env("MARK", "?")} }}})
run_panes()
EOF
done
id_of() { for _ in $(seq 100); do i=$(grep -o 'on [0-9a-f]\{12\}`' "$W/serve$1.log" | tr -d '`' | cut -c4-); [ -n "$i" ] && { echo "$i"; return; }; sleep 0.1; done; }
IDA=$(id_of A); IDB=$(id_of B)
[ -n "$IDA" ] && [ -n "$IDB" ] || fail "the hosts did not come up: $(tail -n 3 "$W"/serve*.log)"
for _ in $(seq 100); do grep -q 'joined the fleet\|attached to hub' "$W/serveA.log" && grep -q 'joined the fleet\|attached to hub' "$W/serveB.log" && break; sleep 0.1; done
app() { local h=$1; shift; local cp=$([ $h = A ] && echo $CA || echo $CB); "$BIN" --root "$W/cli" exec "app $* at 127.0.0.1:$cp key=@$W/root$h/apps/host.key" 2>&1 | tail -1; }
app A set panetest MARK=A > /dev/null; app B set panetest MARK=B PANE_ON=$IDA > /dev/null
app A deploy "$W/appA" | grep -q running || fail "deploy on A"
app B deploy "$W/appB" | grep -q running || fail "deploy on B"

python3 - "$PB" <<'EOF' || exit 1
import http.client, json, re, sys, threading, time, urllib.parse
B = int(sys.argv[1]); bad = []
def req(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", B, timeout=30); c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse(); return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read().decode()
def check(name, ok):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok: bad.append(name)
s, h, html = req("GET", "/")
check("the page through B is A's render", s == 200 and '<p id="who">rendered on A</p>' in html)
check("with A's session cookie", "grain_panetest=" in h.get("set-cookie", ""))
cookie = h["set-cookie"].split(";")[0]
act = re.search(r'data-act="([^"]+)">Add<', html).group(1)
frames = []
def listen():
    c = http.client.HTTPConnection("127.0.0.1", B, timeout=30)
    c.request("GET", "/", headers={"Accept": "text/event-stream", "Cookie": cookie}); r = c.getresponse()
    buf, t0 = b"", time.time()
    while time.time() - t0 < 8 and not any("n=1" in f and "rendered on A" in f for f in frames):
        ch = r.fp.read1(65536)
        if not ch: break
        buf += ch
        while b"\n\n" in buf:
            ev, buf = buf.split(b"\n\n", 1)
            for line in ev.decode().splitlines():
                if line.startswith("data: "): frames.append(json.loads(line[6:]).get("html", ""))
t = threading.Thread(target=listen, daemon=True); t.start(); time.sleep(1)
s, _, text = req("POST", "/_ui/event", urllib.parse.urlencode([("action", act)]), {"X-Grain-UI": "1", "Cookie": cookie, "Content-Type": "application/x-www-form-urlencoded"})
d = json.loads(text)
check("an action through B runs on A", "rendered on A" in d["html"] and "n=1" in d["html"])
t.join(9)
check("B's open page stream gets A's next frame", any("n=1" in f and "rendered on A" in f for f in frames))
s, _, body = req("GET", "/where")
check("a route through B answers on A", body == "route on A")
sys.exit(1 if bad else 0)
EOF

app B set panetest PANE_ON=$IDB > /dev/null; app B restart panetest > /dev/null
for _ in $(seq 50); do out=$(python3 -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:$PB/').read().decode())" 2>/dev/null) && break; sleep 0.1; done
echo "$out" | grep -q '<p id="who">rendered on B</p>' && echo "ok   a pane given to B itself is answered on B" || fail "a pane given to B itself is answered on B"
echo "pane-machine: ok"
