#!/bin/bash
# Acting Things writing to a real MariaDB (throwaway docker mdb109, db thingtgt).
BIN=/repos/mcp-tools/aios/build/aios; R=/tmp/tgt-root; P=13414; N=${N:-20000}
export MYSQL_PWD=$(cat /tmp/gt.pass)
M() { MYSQL_PWD=pw mysql -N -B -h127.0.0.1 -P13309 -uroot thingtgt -e "$1"; }
start() { AIOS_THING_WRITES=1 $BIN --root $R exec "compute serve port=7414 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >>/tmp/tgt-serve.log 2>&1 & echo $! >/tmp/tgt.pid
  until mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1; do sleep 0.3; done; }
q() { mysql -N -B -h127.0.0.1 -P$P -uasql t -e "$1" 2>&1; }
setup_target() {
  MYSQL_PWD=pw mysql -h127.0.0.1 -P13309 -uroot -e "DROP DATABASE IF EXISTS thingtgt; CREATE DATABASE thingtgt"
  M "CREATE TABLE prices (stockid VARCHAR(12) PRIMARY KEY, price DECIMAL(12,2)) ENGINE=InnoDB"
  M "CREATE TABLE staging (stockid VARCHAR(12) PRIMARY KEY, delta DECIMAL(12,2)) ENGINE=InnoDB"
  M "INSERT INTO prices SELECT CONCAT('S', LPAD(seq, 6, '0')), 10 + (seq % 97) FROM seq_1_to_$N"
  M "INSERT INTO staging SELECT CONCAT('S', LPAD(seq, 6, '0')), CAST(seq % 13 AS SIGNED) - 6 + 0.25 FROM seq_1_to_$N"
  M "CREATE TABLE expected AS SELECT p.stockid, p.price + s.delta AS price FROM prices p JOIN staging s USING (stockid)"
}
state() { echo "prices checksum $(M 'SELECT SUM(price), COUNT(*) FROM prices' | tr '\t' '/'), expected $(M 'SELECT SUM(price) FROM expected'), staging $(M "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='thingtgt' AND table_name='staging'")"; }
PROG='fn fire(c) { let n = 0 for r in read_target("SELECT stockid, delta FROM staging ORDER BY stockid") { write("UPDATE prices SET price = price + ? WHERE stockid = ?", r.delta, r.stockid) n = n + 1 if params.fail_at > 0 and n == params.fail_at { write("UPDATE expected SET price = 0") } } return n } fn verify(c, f) { let bad = read_target("SELECT COUNT(*) AS n FROM prices p JOIN expected e USING (stockid) WHERE p.price <> e.price") return {ok: bad[0].n == 0, mismatched: bad[0].n} } fn cleanup(c, f) { return write("DROP TABLE staging") }'
create() { q "CREATE THING $1 AT '$(date '+%F %T')' GRAIN READS x WRITES prices, staging TO $2 PARAMS '{\"fail_at\": $3}' AS '$PROG'"; }
for p in $(ss -ltnp 2>/dev/null | grep ":$P " | grep -o "pid=[0-9]*" | cut -d= -f2); do kill $p; done; sleep 1
rm -rf $R /tmp/tgt-serve.log; mkdir -p $R/sql_db
printf '[profile]\nid=scratch_mdb\nname=throwaway mariadb\nhost=127.0.0.1\nport=13309\nuser=root\npassword=pw\ndatabase=thingtgt\nthings_write=1\n[profile]\nid=unmarked\nname=same server, not opted in\nhost=127.0.0.1\nport=13309\nuser=root\npassword=pw\ndatabase=thingtgt\n' > $R/sql_db/connections.ini
start
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE t"; q "CREATE TABLE x (id INT PRIMARY KEY)"
setup_target
if [ -z "$ONLY4" ]; then
echo "== 1. refused targets"; create e0 unmarked 0 | tail -1; create e0 nosuch 0 | tail -1
echo "== 2. the post, on MariaDB ($N rows)"; echo "   before: $(state)"
create post1 scratch_mdb 0; until ! q "SHOW THINGS" | grep -q "^post1"; do sleep 0.5; done
echo "   after:  $(state)"; q "SHOW THINGS RECEIPTS" | awk -F'\t' '$1=="post1"{print "   receipt: writes " $5 " on " $6 ", " $4 " ms, fire " $7 ", verify " $8 ", cleanup " $9}'
echo "== 3. fire() fails at row 1000: all of it rolled back"; setup_target; echo "   before: $(state)"
create post2 scratch_mdb 1000; until q "SHOW THINGS" | grep "^post2" | grep -q FAILED; do sleep 0.5; done
echo "   after:  $(state)"; q "SHOW THINGS" | awk -F'\t' '$1=="post2"{print "   " $8}' | cut -c1-200
fi
echo "== 4. kill -9 in the middle of the post"; setup_target; echo "   before: $(state)"
create post3 scratch_mdb 0
sleep 5
echo "   5 s in: open transactions $(M "SELECT COUNT(*) FROM information_schema.innodb_trx"), prices as committed $(M 'SELECT SUM(price) FROM prices') - kill -9"; kill -9 $(cat /tmp/tgt.pid); sleep 1
echo "   while down: $(state), open transactions $(M 'SELECT COUNT(*) FROM information_schema.innodb_trx')"
start; until ! q "SHOW THINGS" | grep -q "^post3"; do sleep 0.5; done
echo "   after restart: $(state)"; q "SHOW THINGS RECEIPTS" | awk -F'\t' '$1=="post3"{print "   receipt: writes " $5 " on " $6 ", verify " $8}'
echo "== alerts"; q "SHOW THING ALERTS" | awk -F'\t' '{print "   #" $1, $3, $5, "[" $6 "]"}' | cut -c1-180
kill $(cat /tmp/tgt.pid)
