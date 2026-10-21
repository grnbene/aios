#!/bin/bash
# drive-order-gate.sh -- ORDER BY correctness gate.
#
# Three properties, all of which the engine has broken at least once:
#
#   1. PARITY. A column sorts by its DECLARED type, the way MySQL does. A
#      VARCHAR holding '25' and '-6P0209' orders both by collation; only a
#      numeric column orders numerically. Sorting by the stored type of each
#      individual value instead put 8 numeric-looking stock ids ahead of
#      52,049 text ones.
#   2. NUMERIC. The converse: a numeric column must NOT fall back to
#      collation, or 'ORDER BY n' puts 99 above 10097.
#   3. PREFIX CONSISTENCY. 'ORDER BY k LIMIT L' must be exactly the first L
#      rows of the unlimited sort. A comparator that is not a strict weak
#      ordering breaks this, and partial_sort on a cycle is undefined. Under
#      LIMIT/OFFSET paging a row in the wrong position appears on NO page --
#      the failure is silent data loss, not cosmetic disorder.
#
# The fixture is generated, not committed: numeric-looking values scattered
# through a large text population, which is the shape of ERP stock ids. Text
# values carry numeric prefixes spanning the same range as the numeric ones --
# a narrow prefix band does NOT reproduce the fault.
#
# usage: drive-order-gate.sh <build-dir> [rows]
set -u
BUILD=${1:?usage: drive-order-gate.sh <build-dir> [rows]}
ROWS=${2:-52000}
BIN="$BUILD/aios"
[ -x "$BIN" ] || { echo "FAIL: no aios binary at $BIN"; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" "$ROWS" <<'PY'
import sys
work, rows = sys.argv[1], int(sys.argv[2])
vals = ["-90000032862", "-788502365", "-773017431", "-84668",
        "25", "15111", "1856514", "9275250304", "-", "-6P0209", "-6P0210"]
L = "ABCDEFGHJKLMNPQRSTUVWXYZ"
for i in range(rows - len(vals)):
    n = ((i * 2654435761) % 2000000000) - 1000000000
    vals.append("%d%s%02d" % (n, L[i % len(L)], i % 100))
nums = [99, 10097, 5, 10000, 1, 250, 3, 77, 100000, 42, -7, 0]

out = open(work + "/load.aos", "w")
out.write('sql query "CREATE TABLE t (k TEXT)"\n')
out.write('sql query "CREATE TABLE n (v INT)"\n')
B = 500
for i in range(0, len(vals), B):
    out.write('sql query "INSERT INTO t VALUES %s"\n'
              % ",".join("('%s')" % v for v in vals[i:i+B]))
out.write('sql query "INSERT INTO n VALUES %s"\n' % ",".join("(%d)" % v for v in nums))
out.close()
# utf8mb4_general_ci is case-insensitive; ties break by the raw bytes.
open(work + "/expect_text", "w").write(
    "\n".join(sorted(vals, key=lambda s: (s.lower(), s))) + "\n")
open(work + "/expect_num", "w").write(
    "\n".join(str(v) for v in sorted(nums)) + "\n")
PY

R="$WORK/root"; mkdir -p "$R"
"$BIN" --root "$R" run "$WORK/load.aos" >/dev/null 2>&1 || { echo "FAIL: fixture load"; exit 1; }

q() { "$BIN" --root "$R" exec "sql query \"$1\"" 2>/dev/null \
      | awk -F'|' '/^\|/{gsub(/^ +| +$/,"",$2); print $2}' | tail -n +2; }

FAIL=0
q "SELECT k FROM t ORDER BY k" > "$WORK/full"
LOADED=$(wc -l < "$WORK/full")
[ "$LOADED" -eq "$ROWS" ] || { echo "FAIL: loaded $LOADED rows, expected $ROWS"; exit 1; }

# 1. PARITY -- text column against collation order
if cmp -s "$WORK/expect_text" "$WORK/full"; then
  echo "  text column sorts by collation      ok"
else
  D=$(diff "$WORK/expect_text" "$WORK/full" | grep -c '^<')
  echo "  text column sorts by collation      FAIL ($D rows misplaced)"
  echo "    expected first 5: $(head -5 "$WORK/expect_text" | tr '\n' ' ')"
  echo "    got first 5:      $(head -5 "$WORK/full" | tr '\n' ' ')"
  FAIL=1
fi

# 2. NUMERIC -- numeric column must not fall back to collation
q "SELECT v FROM n ORDER BY v" > "$WORK/gotnum"
if cmp -s "$WORK/expect_num" "$WORK/gotnum"; then
  echo "  numeric column sorts numerically    ok"
else
  echo "  numeric column sorts numerically    FAIL"
  echo "    expected: $(tr '\n' ' ' < "$WORK/expect_num")"
  echo "    got:      $(tr '\n' ' ' < "$WORK/gotnum")"
  FAIL=1
fi

# 3. PREFIX CONSISTENCY -- LIMIT L is the first L of the whole sort
for L in 10 50 100 500 1000 5000; do
  q "SELECT k FROM t ORDER BY k LIMIT $L" > "$WORK/lim"
  head -"$L" "$WORK/full" > "$WORK/exp"
  if cmp -s "$WORK/exp" "$WORK/lim"; then
    echo "  LIMIT $L is a true prefix$(printf '%*s' $((5-${#L})) '')       ok"
  else
    echo "  LIMIT $L is a true prefix$(printf '%*s' $((5-${#L})) '')       FAIL ($(diff "$WORK/exp" "$WORK/lim" | grep -c '^<') of $L differ)"
    FAIL=1
  fi
done

H1=$(q "SELECT k FROM t ORDER BY k LIMIT 1000" | md5sum)
H2=$(q "SELECT k FROM t ORDER BY k LIMIT 1000" | md5sum)
[ "$H1" = "$H2" ] && echo "  repeated query deterministic        ok" \
                  || { echo "  repeated query deterministic        FAIL"; FAIL=1; }

# GROUP BY takes a separate sort path; it has to obey the same rule.
q "SELECT k, COUNT(*) FROM t GROUP BY k ORDER BY k LIMIT 200" > "$WORK/gb"
head -200 "$WORK/expect_text" > "$WORK/gbexp"
if cmp -s "$WORK/gbexp" "$WORK/gb"; then
  echo "  GROUP BY path sorts by collation    ok"
else
  echo "  GROUP BY path sorts by collation    FAIL ($(diff "$WORK/gbexp" "$WORK/gb" | grep -c '^<') differ)"
  FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then echo "=== order gate PASSED ($ROWS rows) ==="
else echo "=== order gate FAILED ($ROWS rows) ==="; fi
exit $FAIL
