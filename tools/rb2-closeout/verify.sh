#!/usr/bin/env bash
# Replays RB2DispatchStatusRollup::decide() line-for-line against local ASQL,
# branch by branch, and asserts each verdict. Proves the close-out RULE is sound
# before any AIOS Thing is allowed anywhere near it.
#
#   ./verify.sh [port]      default 3307
set -uo pipefail
PORT="${1:-3307}"
Q() { mysql -h 127.0.0.1 -P "$PORT" -u root --protocol=TCP -N -B rb2 -e "$1" 2>&1; }

# decide(): returns the reason it would/would not advance, for one dispatch.
decide() {
  local id="$1" d_status d_inv total stillopen
  d_status=$(Q "SELECT status_id FROM dispatch WHERE dispatch_id=$id;")
  [ -z "$d_status" ] && { echo "dispatch_not_found"; return; }
  d_inv=$(Q "SELECT invoiced FROM dispatch WHERE dispatch_id=$id;")
  [ "$d_inv" = "1" ] && { echo "already_invoiced"; return; }
  read -r stillopen total <<<"$(Q "SELECT COALESCE(SUM(CASE WHEN execution_status IN ('pending','en_route') THEN 1 ELSE 0 END),0), COUNT(*) FROM dispatch_stop WHERE dispatch_id=$id;")"
  [ "$total" = "0" ] && { echo "no_stops"; return; }
  [ "$stillopen" != "0" ] && { echo "stops_still_open($stillopen)"; return; }
  [ "$d_status" = "31" ] && { echo "already_delivered"; return; }
  echo "all_stops_finalised"
}

fail=0
check() {
  local id="$1" want="$2" got; got=$(decide "$id")
  if [ "$got" = "$want" ]; then printf '  \033[32mPASS\033[0m  D%s  %s\n' "$id" "$got"
  else printf '  \033[31mFAIL\033[0m  D%s  want=%s got=%s\n' "$id" "$want" "$got"; fail=1; fi
}

echo "RB2DispatchStatusRollup::decide() replayed against ASQL :$PORT"
check 1 "all_stops_finalised"
check 2 "stops_still_open(2)"
check 3 "stops_still_open(1)"
check 4 "all_stops_finalised"
check 5 "already_delivered"
check 6 "already_invoiced"
check 7 "no_stops"

echo
if [ "$fail" = 0 ]; then
  printf '\033[32mall branches agree with the PHP\033[0m — rule is sound, table is right.\n'
else
  printf '\033[31mdivergence — do not build the Thing yet\033[0m\n'
fi
exit $fail
