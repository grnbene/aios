#!/usr/bin/env bash
# grain ps: what runs on this machine, from the records each runtime writes (runtime_registry.hpp). In a registry
# of its own (XDG_RUNTIME_DIR): a `grain serve` and its port, a program's own listen() and its close, a process
# killed with -9 cleared from the list, a normal exit taking its record, an ASQL engine with its Thing, an app host
# with its app - through both `grain ps` and `aios exec "grain ps"`.
#
#   grain-ps-check.sh <grain binary> <aios binary>
set -u
GRAIN=$(realpath "${1:?usage: grain-ps-check.sh <grain> <aios>}")
BIN=$(realpath "${2:?usage: grain-ps-check.sh <grain> <aios>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
mkdir -m 700 "$W/run"
export XDG_RUNTIME_DIR="$W/run"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
failed=0
check() { if [ "$2" = 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=1; fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
ps_json() { "$GRAIN" ps --json; }
has() { ps_json | python3 -c "import json,sys; d=json.load(sys.stdin); r=[x for x in d['registered'] if $1]; sys.exit(0 if r else 1)"; }
wait_for() { for _ in $(seq 100); do has "$1" && return 0; sleep 0.1; done; return 1; }
cd "$W"

# a grain serve
SP=$(free_port)
cat > page.gr <<'EOF'
ui.pane({"title": "p", "view": fn() { return el("p", "hi") }})
run_panes()
EOF
"$GRAIN" serve page.gr --port "$SP" > serve.log 2>&1 & SPID=$!; PIDS+=($SPID)
wait_for "x['pid'] == $SPID and x['kind'] == 'grain serve' and x['what'] == 'page.gr' and any(p['port'] == $SP and p['for'] == 'web' for p in x['ports'])"; check "grain serve is listed with its program and port" $([ $? = 0 ] && echo 1 || echo 0)
"$GRAIN" ps | grep -q "^$SPID .*grain serve .*$SP web .*page.gr"; check "and in the table" $([ $? = 0 ] && echo 1 || echo 0)

# a program's own listen(), then its close
LP=$(free_port)
cat > listen.gr <<EOF
let l = listen($LP)
sleep(1500)
close(l)
sleep(1500)
EOF
"$GRAIN" run listen.gr > /dev/null 2>&1 & LPID=$!; PIDS+=($LPID)
wait_for "x['pid'] == $LPID and x['kind'] == 'grain run' and any(p['port'] == $LP and p['for'] == 'listen' for p in x['ports'])"; check "a program's listen() port" $([ $? = 0 ] && echo 1 || echo 0)
wait_for "x['pid'] == $LPID and not x['ports']"; check "gone from the record when it closes" $([ $? = 0 ] && echo 1 || echo 0)
wait $LPID
sleep 0.2
! has "x['pid'] == $LPID"; check "a program that ends takes its record with it" $([ $? = 0 ] && echo 1 || echo 0)
[ ! -e "$W/run/grain/$LPID.json" ]; check "  (its file is gone)" $([ $? = 0 ] && echo 1 || echo 0)

# kill -9: no chance to clean up; the next reader clears it
kill -9 $SPID; wait $SPID 2>/dev/null
! has "x['pid'] == $SPID"; check "a process killed with -9 is not listed" $([ $? = 0 ] && echo 1 || echo 0)
[ ! -e "$W/run/grain/$SPID.json" ]; check "  and its record was cleared" $([ $? = 0 ] && echo 1 || echo 0)

# an ASQL engine and its Thing
RPC=$(free_port); SQLP=$(free_port)
umask 077; head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > pass; head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > token
"$BIN" --root "$W/asql" exec "compute serve port=$RPC lanes=1 sql=1 mysql_port=$SQLP mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > asql.log 2>&1 < /dev/null & APID=$!; PIDS+=($APID)
for _ in $(seq 200); do grep -q 'answering queries\|listening on 127.0.0.1:'"$SQLP" asql.log && break; sleep 0.05; done
cat > mk.gr <<EOF
let c = db_connect({"host": "127.0.0.1", "port": $SQLP, "user": "app", "password": trim(read_file("$W/pass"))})
for q in ["CREATE DATABASE shop", "USE shop", "CREATE TABLE t (id INT PRIMARY KEY, g VARCHAR(4))", "INSERT INTO t VALUES (1, 'a'), (2, 'b')",
          "CREATE THING tg GRAIN KEY g READS t AS 'fn enroll() { return [\"a\", \"b\"] } fn shared() { return 1 } fn things(ks, sh) { let o = {} for k in ks { o[k] = {\"n\": 1} } return o }'"] {
  let r = db_query(c, q)
  if not r.ok { error(q + ": " + r.error) }
}
print("made")
EOF
"$BIN" --root "$W/cli" exec "grain serve $W/mk.gr" 2>&1 | grep -q made; check "a Thing made on the engine" $([ $? = 0 ] && echo 1 || echo 0)
wait_for "x['pid'] == $APID and x['kind'] == 'asql' and any(p['port'] == $SQLP and p['for'] == 'mysql' for p in x['ports']) and any(t['thing'] == 'tg' and t['things'] == '2' for t in (x.get('things') or []))"; check "the engine is listed with its mysql port and its Thing (2 rows)" $([ $? = 0 ] && echo 1 || echo 0)

# an app host and its app
CTL=$(free_port); WEB=$(free_port)
"$BIN" --root "$W/host" exec "serve port=$CTL" > host.log 2>&1 < /dev/null & HPID=$!; PIDS+=($HPID)
for _ in $(seq 100); do [ -f "$W/host/apps/host.key" ] && grep -q 'control on' host.log && break; sleep 0.1; done
mkdir -p app
echo "let app = {\"name\": \"psapp\", \"main\": \"service.gr\", \"kind\": \"web\", \"port\": $WEB, \"listen\": \"local\"}" > app/app.gr
cp page.gr app/service.gr
"$BIN" --root "$W/cli" exec "app deploy $W/app at 127.0.0.1:$CTL key=@$W/host/apps/host.key" 2>&1 | grep -q running; check "an app deployed to a host" $([ $? = 0 ] && echo 1 || echo 0)
wait_for "x['pid'] == $HPID and x['kind'] == 'app host' and x['root'] == '$W/host/apps' and any(p['port'] == $CTL and p['for'] == 'control' for p in x['ports']) and any(a['name'] == 'psapp' and a['state'] == 'running' and a['port'] == $WEB for a in (x.get('apps') or []))"; check "the app host is listed with its control port, its home and its app running on its port" $([ $? = 0 ] && echo 1 || echo 0)
out=$("$BIN" --root "$W/cli" exec "grain ps" 2>&1)
echo "$out" | grep -q "app   psapp .*running" && echo "$out" | grep -q "thing tg .*2 rows"; check "aios exec \"grain ps\" shows the same" $([ $? = 0 ] && echo 1 || echo 0)
mkdir -p "$W/emptyhome"
out=$(env -u GRAIN_HOME HOME="$W/emptyhome" "$GRAIN" app status 2>&1)
echo "$out" | grep -q "psapp .*running"; check "grain app status, with no ~/.grain, finds the running host by itself" $([ $? = 0 ] && echo 1 || echo 0)
[ "$failed" = 0 ] && echo "grain-ps: ok"
exit $failed
