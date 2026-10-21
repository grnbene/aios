#!/usr/bin/env bash
# asql-check: is this host's ASQL actually serving what its master holds?
# The acceptance test after install-asql.sh, and the thing to run when in doubt.
#
#   asql-check              every whitelisted table against the master (minutes on a big mirror)
#   asql-check TABLE        one table
#   asql-check --quick      service, wire, binlog position and Things only (seconds)
#
# Exit 0 only when the service is up, the wire answers, the binlog is caught up
# (or within --lag-ok bytes), every table is present with matching columns and
# rows, and every declared Thing loaded. Prints what it checked either way.
set -u
CONF=${ASQL_CONF:-/etc/aios/asql.conf}
UNIT=aios-compute.service
QUICK=0; ONLY=""; LAG_OK=${LAG_OK:-0}
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1; shift ;;
    --lag-ok) LAG_OK=${2:?}; shift 2 ;;
    -h|--help) sed -n 2,12p "$0"; exit 0 ;;
    *) ONLY=$1; shift ;;
  esac
done
ok()   { printf '\033[32m[ ok ]\033[0m %s\n' "$*"; }
bad()  { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; FAILED=1; }
FAILED=0

[ -r "$CONF" ] || { bad "cannot read $CONF"; exit 1; }
# shellcheck source=/dev/null
. "$CONF"
: "${MYSQL_SOCKET:=/opt/aios/run/asql.sock}"; : "${MYSQL_USER:=asql}"; : "${MYSQL_PASS_FILE:=/opt/aios/mysql.pass}"
: "${CDC_DB:=}"; : "${AIOS_ROOT:=/opt/aios/root}"

# 1. the service, by its start timestamp and main pid
if command -v systemctl >/dev/null && systemctl cat "$UNIT" >/dev/null 2>&1; then
  pid=$(systemctl show -p MainPID "$UNIT" | cut -d= -f2); since=$(systemctl show -p ExecMainStartTimestamp "$UNIT" | cut -d= -f2-)
  if [ "${pid:-0}" != 0 ] && [ "$(systemctl show -p ActiveState "$UNIT" | cut -d= -f2)" = active ]; then ok "$UNIT active, pid $pid, since $since"
  else bad "$UNIT is not running (pid ${pid:-0}, $(systemctl show -p ActiveState "$UNIT" | cut -d= -f2))"; fi
else
  ok "no systemd unit here (--no-service install); checking the wire only"
fi

# 2. the wire: the unix socket answers as the configured user
if ! command -v mysql >/dev/null; then bad "no mysql client on this host: the wire and table checks need one"; exit 1; fi
export MYSQL_PWD; MYSQL_PWD=$(cat "$MYSQL_PASS_FILE" 2>/dev/null || true)
M=(mysql -S "$MYSQL_SOCKET" -u"$MYSQL_USER" -N -B)
[ -n "$CDC_DB" ] && M+=(-D "$CDC_DB")
if v=$("${M[@]}" -e "SELECT VERSION()" 2>&1); then ok "wire answers on $MYSQL_SOCKET: $v"
else bad "wire does not answer on $MYSQL_SOCKET: $v"; exit 1; fi

# 3. the mirror against its master (the engine asks the master itself)
if [ "$QUICK" = 1 ]; then
  out=$("${M[@]}" -e "SHOW ASQL MIRROR __none__" 2>&1)
else
  out=$("${M[@]}" -e "SHOW ASQL MIRROR ${ONLY:+\`$ONLY\`}" 2>&1)
fi
if printf '%s' "$out" | grep -q "^(binlog)"; then
  bl=$(printf '%s\n' "$out" | grep "^(binlog)")
  state=$(printf '%s' "$bl" | cut -f2); mine=$(printf '%s' "$bl" | cut -f3); theirs=$(printf '%s' "$bl" | cut -f4)
  if [ "$state" = "caught up" ]; then ok "binlog caught up at $mine"
  else
    mp=${mine##*:}; tp=${theirs##*:}; mf=${mine%%:*}; tf=${theirs%%:*}
    if [ "$mf" = "$tf" ] && [ "$tp" -ge "$mp" ] 2>/dev/null && [ $((tp - mp)) -le "$LAG_OK" ]; then ok "binlog within $((tp - mp)) bytes of the master ($mine vs $theirs)"
    else bad "binlog behind: mirror $mine, master $theirs"; fi
  fi
  if [ "$QUICK" = 0 ]; then
    printf '%s\n' "$out" | awk 'BEGIN{FS="\t"} $1 !~ /^\(/ && $2 != "ok" && $2 != "view" {printf "       %-32s %-16s rows mirror %s / master %s   cols %s / %s\n", $1, $2, ($3==""?"-":$3), ($4==""?"-":$4), ($5==""?"-":$5), ($6==""?"-":$6)}'
    sum=$(printf '%s\n' "$out" | grep "^(summary)" | cut -f2)
    case "$sum" in
      *" 0 differ, 0 missing") ok "tables vs master: $sum" ;;
      *) bad "tables vs master: $sum" ;;
    esac
  fi
else
  bad "SHOW ASQL MIRROR failed: $(printf '%s' "$out" | head -2)"
fi

# 4. Things: every kind declared on this host loaded (a kind that failed to load is not listed)
things=$("${M[@]}" -e "SHOW THINGS" 2>/dev/null | cut -f1 | tr '\n' ' ')
declared=$(ls "$AIOS_ROOT/sql_db/things/"*.thing 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.thing$//' | tr '\n' ' ')
missing=""
for d in $declared; do case " $things " in *" $d "*) ;; *) missing="$missing $d";; esac; done
if [ -n "$missing" ]; then bad "Things on disk but not loaded:$missing"
elif [ -n "$declared" ]; then ok "Things loaded: $things"
else ok "no Things declared on this host"; fi

[ "$FAILED" = 0 ] && { ok "all checks passed"; exit 0; } || { bad "some checks failed"; exit 1; }
