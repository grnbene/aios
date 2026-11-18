#!/usr/bin/env bash
# aios honeypot - set up the HUB on the server that runs your aios-webserver. One command, safe to re-run:
#
#   sudo apps/honeypot/deploy/install-hub.sh
#
# It finds your running aios-webserver and its config (or pass --config FILE [--vhost NAME]), then:
#   - builds what's missing (aios, and a static aios-webserver that runs on any x86_64 Linux - handed to your nodes)
#   - creates the fleet secret and a dashboard password (kept in /etc/aios, 0600)
#   - runs the dashboard app under a systemd service, on 127.0.0.1 only
#   - adds `honeypot_hub /honeypot` (and a trap for this server itself) to your vhost - backed up, validated with
#     `aios-webserver --check`, and restored automatically if the check fails - then restarts your aios-webserver
#   - prints the dashboard address, the password, and the one command that adds each of your other servers.
#
# Options: --config FILE  --vhost NAME  --url https://host/honeypot  --password PW  --no-trap  --no-restart
#          --uninstall (removes what this added; keeps all recorded evidence)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
ROOT=/var/lib/aios
APPHOST_PORT=7480
CONFIG=""; VHOST=""; URL=""; PASSWORD=""; TRAP=1; RESTART=1; UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --vhost) VHOST="$2"; shift 2 ;;
    --url) URL="${2%/}"; shift 2 ;;
    --password) PASSWORD="$2"; shift 2 ;;
    --no-trap) TRAP=0; shift ;;
    --no-restart) RESTART=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
BEGIN="# >>> aios-honeypot hub (install-hub.sh; remove with --uninstall)"
END="# <<< aios-honeypot hub"
say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root: sudo $0"

# ---- find the running aios-webserver: its binary, its config, its systemd unit -------------------------------
WS_PID="$(pgrep -o -f '(^|/)aios-webserver ' || true)"
WS_BIN=""; WS_UNIT=""
if [ -n "$WS_PID" ]; then
  WS_BIN="$(readlink -f "/proc/$WS_PID/exe" 2>/dev/null || true)"
  if [ -z "$CONFIG" ]; then
    # args are NUL-separated: [binary, config] (or [binary, --check, config] - not a server, ignore)
    CONFIG="$(tr '\0' '\n' < "/proc/$WS_PID/cmdline" | sed -n '2p')"
    case "$CONFIG" in /*) ;; *) CONFIG="$(readlink -f "/proc/$WS_PID/cwd")/$CONFIG" ;; esac
  fi
  WS_UNIT="$(grep -o '[^/]*\.service' "/proc/$WS_PID/cgroup" 2>/dev/null | head -1 || true)"
fi
[ -n "$CONFIG" ] || die "no running aios-webserver found - pass --config /path/to/your/aios-webserver.conf"
[ -f "$CONFIG" ] || die "config not found: $CONFIG"

# ---- uninstall ------------------------------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
  if grep -qF "$BEGIN" "$CONFIG"; then
    cp -a "$CONFIG" "$CONFIG.aios-bak.$(date +%s)"
    awk -v b="$BEGIN" -v e="$END" 'index($0,b){skip=1;next} index($0,e){skip=0;next} !skip{print}' "$CONFIG" > "$CONFIG.aios-tmp"
    cat "$CONFIG.aios-tmp" > "$CONFIG"; rm -f "$CONFIG.aios-tmp"
    say "removed the hub lines from $CONFIG"
    [ -n "$WS_UNIT" ] && [ "$RESTART" = 1 ] && systemctl restart "$WS_UNIT" && say "restarted $WS_UNIT"
  fi
  if [ -d /run/systemd/system ]; then
    systemctl disable --now aios-apphost >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/aios-apphost.service; systemctl daemon-reload || true
  else
    pkill -f "aios --root $ROOT exec serve" 2>/dev/null || true
  fi
  say "Done. Kept: $ROOT (dashboard data + evidence) and /etc/aios (secret, password)."
  exit 0
fi

# ---- binaries -------------------------------------------------------------------------------------------------
[ "$(uname -m)" = x86_64 ] || warn "this hub is $(uname -m): the node binary it hands out will only run on $(uname -m) servers"
cd "$REPO"
if [ ! -x build/aios ]; then
  say "Building aios (first time only; takes a while)"
  cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build --target aios -j"$(nproc)" >/dev/null
fi
if [ ! -x build-static/aios-webserver ]; then
  say "Building a static aios-webserver for your nodes (first time only; takes a while)"
  cmake -B build-static -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null
  cmake --build build-static --target aios-webserver -j"$(nproc)" >/dev/null
fi
case "$(build-static/aios-webserver --help 2>&1 || true)" in
  *honeypot_hub*) ;;
  *) die "build-static/aios-webserver is too old - delete build-static/ and re-run" ;;
esac
install -m 755 build/aios /usr/local/bin/aios.new && mv -f /usr/local/bin/aios.new /usr/local/bin/aios

# ---- secrets --------------------------------------------------------------------------------------------------
mkdir -p /etc/aios "$ROOT" /var/log/aios
chmod 700 /etc/aios
[ -s /etc/aios/fleet.token ] || { head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n' > /etc/aios/fleet.token; echo >> /etc/aios/fleet.token; }
chmod 600 /etc/aios/fleet.token
TOKEN="$(head -1 /etc/aios/fleet.token)"
if [ -n "$PASSWORD" ]; then printf '%s\n' "$PASSWORD" > /etc/aios/dashboard.password
elif [ ! -s /etc/aios/dashboard.password ]; then head -c 18 /dev/urandom | base64 | tr -d '/+=\n' > /etc/aios/dashboard.password; echo >> /etc/aios/dashboard.password; fi
chmod 600 /etc/aios/dashboard.password
PASSWORD="$(head -1 /etc/aios/dashboard.password)"

# ---- which vhost, and the public URL ---------------------------------------------------------------------------
if [ -z "$VHOST" ]; then
  # prefer the first vhost that listens with ssl, else the first vhost
  VHOST="$(awk '/^[[:space:]]*vhost[[:space:]]/{name=$2} /^[[:space:]]*listen[[:space:]].*ssl/{if(name!=""){print name; exit}}' "$CONFIG")"
  [ -n "$VHOST" ] || VHOST="$(awk '/^[[:space:]]*vhost[[:space:]]/{print $2; exit}' "$CONFIG")"
fi
grep -qE "^[[:space:]]*vhost[[:space:]]+${VHOST}[[:space:]]*\{" "$CONFIG" || die "vhost '$VHOST' not found in $CONFIG (use --vhost NAME)"
if [ -z "$URL" ]; then
  case "$VHOST" in *.*) ;; *) die "can't tell this hub's public address from vhost '$VHOST' - pass --url https://your.domain/honeypot" ;; esac
  if awk -v v="$VHOST" '$1=="vhost"{inv=($2==v)} inv && $1=="listen" && /ssl/{f=1} END{exit !f}' "$CONFIG"; then URL="https://$VHOST/honeypot"
  else URL="http://$VHOST/honeypot"; warn "vhost $VHOST has no ssl listener: nodes will report over plain http"; fi
fi

# ---- the dashboard app, as a service -----------------------------------------------------------------------------
say "Starting the dashboard app host (127.0.0.1 only)"
cat > /etc/systemd/system/aios-apphost.service <<EOF
[Unit]
Description=aios app host (honeypot dashboard)
After=network-online.target
[Service]
ExecStart=/usr/local/bin/aios --root $ROOT exec "serve port=$APPHOST_PORT"
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target
EOF
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload
  systemctl enable aios-apphost >/dev/null 2>&1
  systemctl restart aios-apphost
else
  warn "no systemd: starting the app host in the background now; add it to your init system to survive reboots"
  pkill -f "aios --root $ROOT exec serve" 2>/dev/null || true; sleep 1
  nohup /usr/local/bin/aios --root "$ROOT" exec "serve port=$APPHOST_PORT" >> /var/log/aios/apphost.log 2>&1 &
fi
for _ in $(seq 1 50); do [ -s "$ROOT/host.port" ] && break; sleep 0.2; done
sleep 1
aiosx() { /usr/local/bin/aios --root "$ROOT" exec "$1" 2>&1 | grep -v "^asql:"; }
say "Deploying the dashboard"
rm -rf "$REPO/apps/honeypot/.grain"
aiosx "app deploy $REPO/apps/honeypot" | tail -1
rm -rf "$REPO/apps/honeypot/.grain"
aiosx "app set honeypot FLEET_TOKEN=$TOKEN DASH_PASSWORD=$PASSWORD AIOS_NODE=hub PUBLIC_URL=$URL" >/dev/null
DATA="$(find "$ROOT" -type d -path '*/honeypot/data' | head -1)"
[ -n "$DATA" ] || die "dashboard deployed but its data directory wasn't found under $ROOT"
install -m 644 build-static/aios-webserver "$DATA/aios-webserver.bin"
aiosx "app restart honeypot" >/dev/null
for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:8577/health && break; sleep 0.2; done
case "$(curl -s http://127.0.0.1:8577/health || true)" in
  *ok*) ;;
  *) die "the dashboard app did not come up - see: journalctl -u aios-apphost (or /var/log/aios/apphost.log)" ;;
esac

# ---- wire the hub into your aios-webserver ------------------------------------------------------------------------
{
  BACKUP="$CONFIG.aios-bak.$(date +%s)"
  cp -a "$CONFIG" "$BACKUP"
  # a re-run replaces its own block (so upgrades pick up new lines); nothing outside the markers is touched
  if grep -qF "$BEGIN" "$CONFIG"; then
    awk -v b="$BEGIN" -v e="$END" 'index($0,b){skip=1;next} index($0,e){skip=0;next} !skip{print}' "$CONFIG" > "$CONFIG.aios-tmp"
    cat "$CONFIG.aios-tmp" > "$CONFIG"; rm -f "$CONFIG.aios-tmp"
  fi
  BLOCK="  $BEGIN
  honeypot_hub /honeypot http://127.0.0.1:8577"
  if [ "$TRAP" = 1 ] && ! awk -v v="$VHOST" '$1=="vhost"{inv=($2==v)} inv && $1=="env_honeypot"{f=1} END{exit !f}' "$CONFIG"; then
    BLOCK="$BLOCK
  env_honeypot on
  env_honeypot_node hub
  env_honeypot_store $ROOT/honeypot-store
  env_honeypot_feed /var/log/aios/honeypot.jsonl
  env_honeypot_forward http://127.0.0.1:8577/ingest
  env_honeypot_token_file /etc/aios/fleet.token
  honeypot_path /wp-config\\.php
  honeypot_path /\\.git/config
  honeypot_path \\.\\./|%2[eE]%2[eE]"
  fi
  BLOCK="$BLOCK
  $END"
  awk -v v="$VHOST" -v block="$BLOCK" '{print} !done && $1=="vhost" && $2==v && /\{/ {print block; done=1}' "$CONFIG" > "$CONFIG.aios-tmp"
  cat "$CONFIG.aios-tmp" > "$CONFIG"; rm -f "$CONFIG.aios-tmp"
  if ! build-static/aios-webserver --check "$CONFIG" >/dev/null; then
    cp -a "$BACKUP" "$CONFIG"
    build-static/aios-webserver --check "$CONFIG" || true
    die "the updated config did not validate; $CONFIG was restored unchanged"
  fi
  say "Hub lines in place in vhost $VHOST of $CONFIG (backup: $BACKUP)"
}

# your aios-webserver must be this build to understand the lines above: replace it if it differs (backup kept)
if [ -n "$WS_BIN" ] && ! cmp -s "$WS_BIN" build-static/aios-webserver; then
  cp -a "$WS_BIN" "$WS_BIN.aios-bak"
  install -m 755 build-static/aios-webserver "$WS_BIN.new" && mv -f "$WS_BIN.new" "$WS_BIN"
  say "Upgraded $WS_BIN (previous kept as $WS_BIN.aios-bak)"
elif [ -z "$WS_BIN" ]; then
  install -m 755 build-static/aios-webserver /usr/local/bin/aios-webserver.new && mv -f /usr/local/bin/aios-webserver.new /usr/local/bin/aios-webserver
fi
if [ "$RESTART" = 1 ] && [ -n "$WS_UNIT" ]; then
  systemctl restart "$WS_UNIT" && say "Restarted $WS_UNIT"
  sleep 2
else
  warn "restart your aios-webserver now so it picks up the hub (it isn't a systemd service I can restart for you)"
fi

# ---- check it from the outside, the way a node will ---------------------------------------------------------------
CHECK="$(curl -sk --max-time 10 "$URL/health" || true)"
case "$CHECK" in
  *ok*) say "Hub answers at $URL" ;;
  *) warn "$URL/health didn't answer yet (DNS, firewall, or the restart above) - check it from another machine" ;;
esac

cat <<EOF

  ┌───────────────────────────────────────────────────────────────────────────────
  │ Dashboard:  $URL/
  │ Password:   $PASSWORD      (any username; kept in /etc/aios/dashboard.password)
  │
  │ Add a server (nginx or Apache) - run on that server, set NAME:
  │   curl -fsSL -H "X-Fleet-Token: $TOKEN" $URL/join.sh | sudo bash -s -- --name NAME
  │
  │ The same command is on the dashboard ("Add a server"). It contains the fleet secret.
  └───────────────────────────────────────────────────────────────────────────────
EOF
