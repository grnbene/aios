#!/usr/bin/env bash
# Install or upgrade ASQL on one host. Idempotent and re-runnable.
# Run it on the target, or from anywhere with --host <alias> to ship this
# package over ssh and run it there.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

PREFIX=/opt/aios
CONF_DIR=/etc/aios
CONF=$CONF_DIR/asql.conf
UNIT_NAME=aios-compute.service
UNIT=/etc/systemd/system/$UNIT_NAME
RUN_USER=aios
RUN_GROUP=aios
MEMORY_MAX=${MEMORY_MAX:-10G}
MEMORY_HIGH=${MEMORY_HIGH:-8.5G}

TARGET_HOST=""
DRY_RUN=0
NO_START=0
NO_SERVICE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --host)       TARGET_HOST=${2:?--host needs a value}; shift 2 ;;
    --dry-run)    DRY_RUN=1; shift ;;
    --no-start)   NO_START=1; shift ;;
    --no-service) NO_SERVICE=1; shift ;;
    -h|--help)
      echo "Install or upgrade ASQL on one host. Idempotent and re-runnable."
      echo
      echo "Usage: $0 [--host <ssh-alias>] [--dry-run] [--no-start] [--no-service]"
      echo
      echo "  --host        ship this package to <ssh-alias> and install there"
      echo "  --dry-run     print every change without making one"
      echo "  --no-start    install and enable, but do not start the service"
      echo "  --no-service  files, user and conf only, no systemd unit: for a"
      echo "                container or a host without systemd; run"
      echo "                /opt/aios/bin/asql-serve yourself (as user aios)"
      exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY_RUN" = 1 ]; then printf '   would: %s\n' "$*"; else "$@"; fi; }

if [ -n "$TARGET_HOST" ]; then
  PKG_NAME=$(basename "$HERE")
  say "shipping $PKG_NAME to $TARGET_HOST"
  ssh -o BatchMode=yes -o ConnectTimeout=15 "$TARGET_HOST" true \
    || die "cannot reach $TARGET_HOST over ssh"
  TAR_OPTS=""
  tar --help 2>&1 | grep -q 'no-mac-metadata' && TAR_OPTS="--no-mac-metadata --no-xattrs"
  COPYFILE_DISABLE=1 tar -C "$(dirname "$HERE")" -cz $TAR_OPTS "$PKG_NAME" \
    | ssh -o BatchMode=yes "$TARGET_HOST" "rm -rf /tmp/$PKG_NAME && tar -C /tmp -xzm"
  ARGS=""
  [ "$DRY_RUN"    = 1 ] && ARGS="$ARGS --dry-run"
  [ "$NO_START"   = 1 ] && ARGS="$ARGS --no-start"
  [ "$NO_SERVICE" = 1 ] && ARGS="$ARGS --no-service"
  exec ssh -o BatchMode=yes "$TARGET_HOST" \
    "MEMORY_MAX='$MEMORY_MAX' MEMORY_HIGH='$MEMORY_HIGH' bash /tmp/$PKG_NAME/install-asql.sh $ARGS"
fi

say "preflight"

[ -f "$HERE/bin/aios" ] || die "package is incomplete: no bin/aios beside this script"
[ -f "$HERE/MANIFEST" ] || die "package is incomplete: no MANIFEST"

[ "$(id -u)" = 0 ]         || die "must run as root"
[ "$(uname -m)" = x86_64 ] || die "this package is x86_64; host is $(uname -m)"
if [ "$NO_SERVICE" = 0 ] && ! command -v systemctl >/dev/null; then
  die "no systemd on this host: install with --no-service and run /opt/aios/bin/asql-serve yourself"
fi

HOST_GLIBC=$(ldd --version 2>&1 | awk 'NR==1{print $NF}')
NEED_GLIBC=$(grep '^glibc_floor=' "$HERE/MANIFEST" | cut -d= -f2 | tr -d 'GLIBC_')
: "${NEED_GLIBC:=2.17}"
if [ "$(printf '%s\n%s\n' "$NEED_GLIBC" "$HOST_GLIBC" | sort -V | head -1)" != "$NEED_GLIBC" ]; then
  die "binary needs glibc >= $NEED_GLIBC, host has $HOST_GLIBC"
fi
say "glibc $HOST_GLIBC satisfies the binary's $NEED_GLIBC floor"

WANT_SHA=$(grep '^sha256=' "$HERE/MANIFEST" | cut -d= -f2)
GOT_SHA=$(sha256sum "$HERE/bin/aios" | cut -d' ' -f1)
[ "$WANT_SHA" = "$GOT_SHA" ] || die "bin/aios does not match MANIFEST sha256 (corrupt transfer?)"
say "sha256 matches the manifest"

# A binary that cannot resolve its symbols here must never reach /opt/aios/bin.
# --help is the only safe probe: an unrecognised flag makes the CLI boot for real.
# Probed where it lies: /tmp is noexec on a hardened host (and in a container).
PROBE=$(mktemp -d); trap 'rm -rf "$PROBE"' EXIT
chmod +x "$HERE/bin/aios"
if ! "$HERE/bin/aios" --help >/dev/null 2>"$PROBE/err"; then
  die "load probe failed on this host: $(head -3 "$PROBE/err")"
fi
say "load probe passed (--help ran clean)"

SYSTEMD_VER=$(systemctl --version 2>/dev/null | awk 'NR==1{print $2}')
if [ "$NO_SERVICE" = 1 ]; then
  READWRITEPATHS=""; MEMORYLIMITS=""
elif [ "$SYSTEMD_VER" -ge 231 ] 2>/dev/null; then
  READWRITEPATHS="ReadWritePaths=$PREFIX/root"
  MEMORYLIMITS="MemoryMax=$MEMORY_MAX
MemoryHigh=$MEMORY_HIGH"
else
  READWRITEPATHS="# ReadWritePaths unsupported on systemd $SYSTEMD_VER (needs >= 231)"
  MEMORYLIMITS="# MemoryMax/MemoryHigh unsupported on systemd $SYSTEMD_VER; cgroup-v1 hard limit instead
MemoryLimit=$MEMORY_MAX"
  warn "systemd $SYSTEMD_VER: omitting ReadWritePaths, which it would reject as unknown"
fi

[ "$DRY_RUN" = 1 ] && say "DRY RUN — nothing below is executed"

if ! getent group "$RUN_GROUP" >/dev/null; then run groupadd --system "$RUN_GROUP"; fi
if ! getent passwd "$RUN_USER" >/dev/null; then
  run useradd --system --gid "$RUN_GROUP" --home-dir "$PREFIX" \
              --shell /sbin/nologin "$RUN_USER"
fi
run install -d -m 755 "$PREFIX" "$PREFIX/bin"
run install -d -m 750 -o "$RUN_USER" -g "$RUN_GROUP" "$PREFIX/root"
# The unix socket lives here; the directory must be traversable by the PHP/web
# user, which is not $RUN_USER.
run install -d -m 755 -o "$RUN_USER" -g "$RUN_GROUP" "$PREFIX/run"
# The service runs as $RUN_USER, so it must be able to TRAVERSE this
# directory, not just read the file inside it. root:root 0750 silently denies
# that and the engine exits 78 on every start.
run install -d -m 750 -o root -g "$RUN_GROUP" "$CONF_DIR"

for s in cdc.pass mysql.pass compute.token; do
  if [ ! -e "$PREFIX/$s" ]; then
    run install -m 600 -o "$RUN_USER" -g "$RUN_GROUP" /dev/null "$PREFIX/$s"
    warn "created empty $PREFIX/$s — populate it before the service will work"
  fi
done

BACKUP=""
if [ -e "$PREFIX/bin/aios" ]; then
  BACKUP="$PREFIX/bin/aios.bak-$(date +%Y%m%d-%H%M%S)"
  say "backing up current binary to $(basename "$BACKUP")"
  run cp -p "$PREFIX/bin/aios" "$BACKUP"
fi
run install -m 755 "$HERE/bin/aios"       "$PREFIX/bin/aios"
run install -m 755 "$HERE/bin/asql-serve" "$PREFIX/bin/asql-serve"
for t in asql-export.sh asql-import.sh; do
  [ -e "$HERE/bin/$t" ] && run install -m 755 "$HERE/bin/$t" "$PREFIX/bin/$t"
done
if [ -e "$HERE/bin/aios-asql-top" ]; then
  run install -m 755 "$HERE/bin/aios-asql-top" "$PREFIX/bin/aios-asql-top"
fi
if [ -e "$HERE/bin/aios-asql-console" ]; then
  run install -m 755 "$HERE/bin/aios-asql-console" "$PREFIX/bin/aios-asql-console"
fi
[ -e "$HERE/bin/asql-check.sh" ] && run install -m 755 "$HERE/bin/asql-check.sh" "$PREFIX/bin/asql-check"
# The web server: binary and example conf always; its unit only once an
# operator has written /etc/aios/webserver.conf (nothing listens by accident).
WEB_UNIT=/etc/systemd/system/aios-webserver.service
if [ -e "$HERE/bin/aios-webserver" ]; then
  [ -e "$PREFIX/bin/aios-webserver" ] && run cp -p "$PREFIX/bin/aios-webserver" "$PREFIX/bin/aios-webserver.bak-$(date +%Y%m%d-%H%M%S)"
  run install -m 755 "$HERE/bin/aios-webserver" "$PREFIX/bin/aios-webserver"
  [ -e "$CONF_DIR/webserver.conf.example" ] || run install -m 644 "$HERE/conf/webserver.conf.example" "$CONF_DIR/webserver.conf.example"
fi
# The Things declarations shipped with this version, for the operator to declare.
if [ -d "$HERE/things" ]; then
  run install -d -m 755 "$PREFIX/things"
  for f in "$HERE/things/"*; do run install -m 644 "$f" "$PREFIX/things/$(basename "$f")"; done
fi

if [ ! -e "$CONF" ]; then
  run install -m 640 -o root -g "$RUN_GROUP" "$HERE/conf/asql.conf.example" "$CONF"
  warn "seeded $CONF from the example — review CDC_DB and CDC_TABLES before starting"
else
  say "keeping existing $CONF"
  # A conf pinned to some other binary (AIOS_BIN=/opt/aios/bin/aios.<hash>, a
  # hand deploy) would keep running that one after this install; the package's
  # binary is the one meant from here on, so the pin is re-pointed, conf backed up.
  PINNED=$(sed -n 's/^AIOS_BIN=//p' "$CONF" | tail -1)
  if [ -n "$PINNED" ] && [ "$PINNED" != "$PREFIX/bin/aios" ]; then
    say "conf pins AIOS_BIN=$PINNED; re-pointing it at $PREFIX/bin/aios (backup kept)"
    if [ "$DRY_RUN" = 1 ]; then printf '   would: set AIOS_BIN=%s/bin/aios in %s\n' "$PREFIX" "$CONF"
    else cp -p "$CONF" "$CONF.bak-$(date +%Y%m%d-%H%M%S)"; sed -i "s|^AIOS_BIN=.*|AIOS_BIN=$PREFIX/bin/aios|" "$CONF"; fi
  fi
  if ! grep -q '^MYSQL_SOCKET=' "$CONF"; then
    say "adding MYSQL_SOCKET to $CONF"
    if [ "$DRY_RUN" = 1 ]; then printf '   would: append MYSQL_SOCKET=%s/run/asql.sock\n' "$PREFIX"
    else printf 'MYSQL_SOCKET=%s/run/asql.sock\n' "$PREFIX" >> "$CONF"; fi
  fi
fi

if [ "$NO_SERVICE" = 1 ]; then
  say "--no-service: no systemd unit written"
  [ "$DRY_RUN" = 1 ] && { say "dry run complete — no changes were made"; exit 0; }
  echo
  echo "  ASQL $(grep '^version=' "$HERE/MANIFEST" | cut -d= -f2) installed on $(hostname -s)"
  echo "  edit the conf:      $CONF   (CDC_HOST/CDC_USER/CDC_DB, and the two password files)"
  echo "  see the command:    $PREFIX/bin/asql-serve --print"
  echo "  run the engine:     su -s /bin/sh $RUN_USER -c 'AIOS_NO_ACCEL=1 $PREFIX/bin/asql-serve'"
  echo "  then connect:       $PREFIX/bin/aios-asql-console"
  exit 0
fi

UNIT_BACKUP=""
if [ -e "$UNIT" ] && [ "$DRY_RUN" = 0 ]; then
  UNIT_BACKUP="$UNIT.bak-$(date +%Y%m%d-%H%M%S)"
  cp -p "$UNIT" "$UNIT_BACKUP"
  say "backing up current unit to $(basename "$UNIT_BACKUP")"
fi

say "writing $UNIT"
if [ "$DRY_RUN" = 1 ]; then
  printf '   would: render %s (systemd %s)\n' "$UNIT" "$SYSTEMD_VER"
else
  awk -v u="$RUN_USER" -v g="$RUN_GROUP" -v ml="$MEMORYLIMITS" \
      -v rw="$READWRITEPATHS" '
    { gsub(/@RUN_USER@/, u); gsub(/@RUN_GROUP@/, g);
      gsub(/@MEMORYLIMITS@/, ml);
      gsub(/@READWRITEPATHS@/, rw); print }
  ' "$HERE/systemd/aios-compute.service.in" > "$UNIT"
  chmod 644 "$UNIT"
  if systemd-analyze verify "$UNIT" 2>&1 | grep -q 'Unknown lvalue'; then
    systemd-analyze verify "$UNIT" 2>&1 | grep 'Unknown lvalue' >&2
    die "generated unit has directives this systemd rejects"
  fi
fi

# Check as the service user, not as root: root can read a conf inside a
# directory the service user cannot even traverse.
as_service_user() {   # a minimal host may have no sudo; su is always there
  if command -v sudo >/dev/null; then sudo -u "$RUN_USER" "$@"
  else su -s /bin/sh "$RUN_USER" -c "$*"; fi
}
if [ "$DRY_RUN" = 0 ] && ! as_service_user test -r "$CONF"; then
  die "$CONF is not readable by $RUN_USER — check the mode and group on $CONF_DIR"
fi
say "$CONF is readable by $RUN_USER"

if [ -e "$HERE/bin/aios-webserver" ] && [ "$NO_SERVICE" = 0 ]; then
  if [ -e "$CONF_DIR/webserver.conf" ]; then
    say "writing $WEB_UNIT ($CONF_DIR/webserver.conf exists)"
    run install -m 644 "$HERE/systemd/aios-webserver.service.in" "$WEB_UNIT"
    WEB_ENABLE=1
  else
    say "no $CONF_DIR/webserver.conf: aios-webserver installed, not enabled (copy the example and re-run)"
    WEB_ENABLE=0
  fi
fi

run systemctl daemon-reload
run systemctl enable "$UNIT_NAME"
[ "${WEB_ENABLE:-0}" = 1 ] && run systemctl enable aios-webserver.service

if [ "$NO_START" = 1 ]; then
  say "--no-start: leaving the service alone. Start it with: systemctl start $UNIT_NAME"
  exit 0
fi
if [ "$DRY_RUN" = 1 ]; then
  say "dry run complete — no changes were made"
  exit 0
fi

# is-active was already 'active' before the restart, so it proves nothing;
# only a changed ExecMainStartTimestamp shows the new process actually came up.
unit_prop() { systemctl show -p "$1" "$UNIT_NAME" 2>/dev/null | cut -d= -f2-; }
BEFORE=$(unit_prop ExecMainStartTimestamp)
say "restarting $UNIT_NAME"
if ! systemctl restart "$UNIT_NAME" 2>"$PROBE/restart.err"; then
  warn "restart command failed: $(head -3 "$PROBE/restart.err")"
fi
sleep 5
AFTER=$(unit_prop ExecMainStartTimestamp)
MAINPID=$(unit_prop MainPID); : "${MAINPID:=0}"

if [ -z "$AFTER" ] || [ "$AFTER" = "$BEFORE" ] || [ "$MAINPID" = 0 ]; then
  warn "service did not come up (start timestamp unchanged: '${BEFORE:-none}')"
  journalctl -u "$UNIT_NAME" --since '60 sec ago' --no-pager | tail -20 >&2 || true
  if [ -n "$BACKUP" ] || [ -n "$UNIT_BACKUP" ]; then
    # Restore the unit too: if the unit is what broke, putting the old binary
    # back under the new unit leaves the service just as dead.
    [ -n "$BACKUP" ] && { warn "restoring binary $(basename "$BACKUP")"; cp -p "$BACKUP" "$PREFIX/bin/aios"; }
    [ -n "$UNIT_BACKUP" ] && { warn "restoring unit $(basename "$UNIT_BACKUP")"; cp -p "$UNIT_BACKUP" "$UNIT"; systemctl daemon-reload; }
    systemctl reset-failed "$UNIT_NAME" 2>/dev/null || true
    systemctl restart "$UNIT_NAME" || true
    sleep 5
    if [ "$(unit_prop ActiveState)" = active ]; then
      die "install rolled back; previous binary and unit restored, service is active again"
    fi
    die "install rolled back but the service is STILL NOT ACTIVE — needs hands on $(hostname -s)"
  fi
  die "install failed and there was nothing to roll back to"
fi

say "started: pid $MAINPID at $AFTER"
if [ "${WEB_ENABLE:-0}" = 1 ]; then
  WB=$(systemctl show -p ExecMainStartTimestamp aios-webserver.service | cut -d= -f2-)
  run systemctl restart aios-webserver.service || warn "aios-webserver did not restart: $(systemctl status aios-webserver --no-pager 2>&1 | tail -3)"
  sleep 2
  WA=$(systemctl show -p ExecMainStartTimestamp aios-webserver.service | cut -d= -f2-)
  [ "$WA" != "$WB" ] && say "aios-webserver started at $WA" || warn "aios-webserver start timestamp unchanged"
fi
say "ASQL $(grep '^version=' "$HERE/MANIFEST" | cut -d= -f2) installed on $(hostname -s)"
echo
echo "  effective command:  $PREFIX/bin/asql-serve --print"
echo "  logs:               journalctl -u $UNIT_NAME -f"
echo "  acceptance:         $PREFIX/bin/asql-check        (service, wire, every table vs the master, Things)"
