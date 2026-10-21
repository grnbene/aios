#!/bin/bash
BIN=/repos/mcp-tools/aios/build/aios; R=/tmp/sqlsat-root; P=13416
export MYSQL_PWD=$(cat /tmp/gt.pass)
for p in $(ss -ltnp 2>/dev/null | grep ":$P " | grep -o "pid=[0-9]*" | cut -d= -f2); do kill $p; done; sleep 1; rm -rf $R; mkdir -p $R
$BIN --root $R exec "compute serve port=7416 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >/tmp/sqlsat.log 2>&1 & echo $! > /tmp/sqlsat.pid
until mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1; do sleep 0.3; done
q() { mysql -N -B -h127.0.0.1 -P$P -uasql t -e "$1" 2>&1; }
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE t"
echo "== 1. inline, satisfiable"
q "CALL SAT('p cnf 3 3\n1 -2 0\n2 3 0\n-1 -3 0')" | cut -f1-4,9
echo "== 2. inline, unsatisfiable (3 pigeons, 2 holes)"
q "CALL SAT('p cnf 6 9\n1 2 0\n3 4 0\n5 6 0\n-1 -3 0\n-1 -5 0\n-3 -5 0\n-2 -4 0\n-2 -6 0\n-4 -6 0')" | cut -f1-6
echo "== 3. from a live table"
q "CREATE TABLE cnf (id INT PRIMARY KEY, lits VARCHAR(100))"
q "INSERT INTO cnf VALUES (1, '1 2'), (2, '-1 3'), (3, '-2 -3'), (4, '-3 1')"
q "CALL SAT FROM (SELECT lits FROM cnf)" | cut -f1-4,9
q "INSERT INTO cnf VALUES (5, '-1')"
q "CALL SAT FROM (SELECT lits FROM cnf)" | cut -f1-4
echo "== 4. a Thing that solves over live data: can each day be staffed?"
q "CREATE TABLE shifts (id INT PRIMARY KEY, day VARCHAR(10), shift VARCHAR(10))"
q "CREATE TABLE avail (id INT PRIMARY KEY, day VARCHAR(10), shift VARCHAR(10), person VARCHAR(10))"
q "INSERT INTO shifts VALUES (1,'mon','am'),(2,'mon','pm'),(3,'tue','am'),(4,'tue','pm')"
q "INSERT INTO avail VALUES (1,'mon','am','ana'),(2,'mon','pm','ana'),(3,'mon','pm','bo'),(4,'tue','am','cy'),(5,'tue','pm','cy'),(6,'tue','pm','bo')"
mysql -h127.0.0.1 -P$P -uasql t <<'SQL'
CREATE THING staffing GRAIN KEY day READS shifts, avail WATCH shifts(day), avail(day) AS '
fn enroll() { let o = [] for r in read("SELECT DISTINCT day FROM shifts") { push(o, r.day) } return o }
fn shared() { return 1 }
fn things(ks, sh) {
  let out = {}
  for d in ks {
    let opts = read("SELECT a.id, a.shift, a.person FROM avail a JOIN shifts s ON s.day = a.day AND s.shift = a.shift WHERE a.day = ? ORDER BY a.id", d)
    let shifts = read("SELECT shift FROM shifts WHERE day = ? ORDER BY shift", d)
    if len(shifts) == 0 { continue }
    let cls = []
    for s in shifts { let c = [] for o in opts { if o.shift == s.shift { push(c, o.id) } } push(cls, c) }
    for i in range(len(opts)) { for j in range(i + 1, len(opts)) {
      let a = opts[i] let b = opts[j]
      if a.shift == b.shift or a.person == b.person { push(cls, [0 - a.id, 0 - b.id]) } } }
    let ans = sat(cls)
    let v = {feasible: ans.status == "SAT", verified: ans.verified}
    if ans.status == "SAT" { let plan = [] for o in opts { if ans.model[o.id - 1] > 0 { push(plan, o.shift + ":" + o.person) } } v.plan = join(plan, " ") }
    else { v.alert = "cannot staff " + d }
    out[d] = v
  }
  return out
}'
SQL
q "SHOW THINGS staffing" | cut -f1-5
echo "   bo becomes unavailable on tuesday:"
q "DELETE FROM avail WHERE id = 6"
q "SHOW THINGS staffing" | cut -f1-5
q "DELETE FROM avail WHERE id = 5"
echo "   and cy can no longer do tuesday pm:"
q "SHOW THINGS staffing" | cut -f1-5
q "SHOW THING ALERTS" | cut -f3-6
kill $(cat /tmp/sqlsat.pid)
