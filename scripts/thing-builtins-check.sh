#!/usr/bin/env bash
# thing_rows and a ui.pane "watch": {"thing", "db"} against a real ASQL: a local engine (compute serve sql=1, no
# MariaDB behind it), a grain Thing made over the wire, a program reading it with thing_rows, and an app host whose
# pane watches the Thing - a write to the table reaches the open page with no action.
#
#   thing-builtins-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-builtins-check.sh <aios binary>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
fail() { echo "FAIL $*"; exit 1; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
RPC=$(free_port); SQLP=$(free_port); CTL=$(free_port); WEB=$(free_port)
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/pass"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/token"
"$BIN" --root "$W/asql" exec "compute serve port=$RPC lanes=1 sql=1 mysql_port=$SQLP mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$W/asql.log" 2>&1 < /dev/null & PIDS+=($!)
for _ in $(seq 200); do grep -q 'listening on 127.0.0.1:'"$SQLP" "$W/asql.log" && break; sleep 0.05; done
grep -q 'listening on 127.0.0.1:'"$SQLP" "$W/asql.log" || fail "ASQL did not start: $(tail -n 3 "$W/asql.log")"

# the settings every program here connects with (the password from its file, never on a command line)
cat > "$W/cfg.gr" <<EOF
let CFG = {"host": "127.0.0.1", "port": $SQLP, "user": "app", "password": trim(read_file("$W/pass"))}
EOF

cat > "$W/setup.gr" <<'EOF'
import "cfg.gr"
let c = db_connect(CFG)
if not c.ok { error("connect: " + c.error) }
for q in ["CREATE DATABASE shop", "USE shop",
          "CREATE TABLE touches (id INT PRIMARY KEY, grp VARCHAR(8), uid VARCHAR(20))",
          "INSERT INTO touches VALUES (1, 'x', 'ann'), (2, 'x', 'bob'), (3, 'it''s', 'cy')"] {
  let r = db_query(c, q)
  if not r.ok { error(q + ": " + r.error) }
}
// a grain Thing: per group, its people (a list) and a count
// (aios's db_query takes no parameters: the program goes in the statement, which has no ' or \ in it)
let prog = "fn enroll() { return unique(map(read_raw_with(\"SELECT grp FROM touches\", nil), fn(r) { return r.grp })) }\n" +
  "fn shared() { return 1 }\n" +
  "fn things(ks, sh) { let out = {} for r in read_raw(\"SELECT grp, uid FROM touches WHERE grp IN (?)\", ks) { " +
  "let g = get_or_put(out, r.grp, {\"people\": [], \"n\": 0}) push(g.people, r.uid) g.n += 1 } return out }\n"
let r = db_query(c, "CREATE THING tt GRAIN KEY grp READS touches AS '" + prog + "'")
if not r.ok { error("CREATE THING: " + r.error) }
print("setup ok")
EOF
cat > "$W/rows.gr" <<'EOF'
import "cfg.gr"
let c = db_connect(CFG)
db_query(c, "USE shop")
let all = thing_rows(c, "tt")
let x = find_if(all, fn(r) { return r.grp == "x" })
assert_true(len(all) == 2, "two groups")
assert_true(type(x.people) == "list" and sort(x.people) == ["ann", "bob"], "a list column comes back a list")
let q = thing_rows(c, "tt", ["it's"])
assert_true(len(q) == 1 and q[0].people == ["cy"], "a key with a quote in it")
assert_true(thing_rows(c, "tt", []) == [], "no keys, no rows")
assert_true(not pcall(thing_rows, c, "tt; DROP TABLE touches", nil).ok, "a name that is not one is refused")
assert_true(not pcall(thing_rows, c, "nosuch", nil).ok, "a Thing that is not there is an error")
print("thing_rows ok")
EOF
cd "$W"
"$BIN" --root "$W/cli" exec "grain serve $W/setup.gr" 2>&1 | grep -q 'setup ok' || fail "setup: $("$BIN" --root "$W/cli" exec "grain serve $W/setup.gr" 2>&1 | head -n 4)"
out=$("$BIN" --root "$W/cli" exec "grain serve $W/rows.gr" 2>&1)
echo "$out" | grep -q 'thing_rows ok' && echo "ok   thing_rows: list columns decoded, a quoted key, refusals" || fail "thing_rows: $out"

# a pane watching the Thing, on an app host
"$BIN" --root "$W/host" exec "serve port=$CTL" > "$W/host.log" 2>&1 < /dev/null & PIDS+=($!)
for _ in $(seq 100); do [ -f "$W/host/apps/host.key" ] && grep -q 'control on' "$W/host.log" && break; sleep 0.1; done
mkdir -p "$W/app"
cp "$W/cfg.gr" "$W/app/cfg.gr"
cat > "$W/app/app.gr" <<EOF
let app = {"name": "watchtest", "main": "service.gr", "kind": "web", "port": $WEB, "listen": "local"}
EOF
cat > "$W/app/service.gr" <<'EOF'
import "cfg.gr"
let db = merge(CFG, {"db": "shop"})
let conn = nil
ui.pane({"title": "Watch", "watch": {"thing": "tt", "db": db, "timeout": 5},
  "view": fn() {
    if conn == nil { conn = db_connect(db) }
    let x = find_if(thing_rows(conn, "tt", ["x"]), fn(r) { return true })
    return el("p", {"id": "n"}, "x has " + str(x == nil ? 0 : x.n))
  }})
run_panes()
EOF
"$BIN" --root "$W/cli" exec "app deploy $W/app at 127.0.0.1:$CTL key=@$W/host/apps/host.key" 2>&1 | grep -q running || fail "deploy: $(tail -n 5 "$W/host/apps/apps/watchtest/log" 2>/dev/null)"
cat > "$W/insert.gr" <<'EOF'
import "cfg.gr"
let c = db_connect(CFG)
db_query(c, "USE shop")
let r = db_query(c, "INSERT INTO touches VALUES (4, 'x', 'dee')")
if not r.ok { error(r.error) }
print("inserted")
EOF
python3 - "$WEB" "$BIN" "$W" <<'EOF' || exit 1
import http.client, json, subprocess, sys, threading, time
web, binp, w = int(sys.argv[1]), sys.argv[2], sys.argv[3]
c = http.client.HTTPConnection("127.0.0.1", web, timeout=20); c.request("GET", "/"); r = c.getresponse()
cookie = r.getheader("Set-Cookie").split(";")[0]; html = r.read().decode()
ok = '<p id="n">x has 2</p>' in html
print(("ok   " if ok else "FAIL ") + "the view reads the Thing with thing_rows")
frames = []
def listen():
    c = http.client.HTTPConnection("127.0.0.1", web, timeout=20)
    c.request("GET", "/", headers={"Accept": "text/event-stream", "Cookie": cookie}); r = c.getresponse()
    buf, t0 = b"", time.time()
    while time.time() - t0 < 12 and not any("x has 3" in f[1] for f in frames):
        ch = r.fp.read1(65536)
        if not ch: break
        buf += ch
        while b"\n\n" in buf:
            ev, buf = buf.split(b"\n\n", 1)
            for line in ev.decode().splitlines():
                if line.startswith("data: "): frames.append((time.time(), json.loads(line[6:]).get("html", "")))
t = threading.Thread(target=listen, daemon=True); t.start(); time.sleep(1.5)
t0 = time.time()
subprocess.run([binp, "--root", w + "/cli", "exec", "grain serve " + w + "/insert.gr"], capture_output=True)
t.join(13)
hit = [f for f in frames if "x has 3" in f[1]]
print(("ok   " if hit else "FAIL ") + "a write to the table reached the open page through the watch" + (" (%.0f ms after the insert began)" % ((hit[0][0] - t0) * 1000) if hit else ""))
sys.exit(0 if ok and hit else 1)
EOF
echo "thing-builtins: ok"
