#!/usr/bin/env bash
# EXPLAIN THING <name> '<key>': the source rows that make up one Thing (its own query, the key's rows, no GROUP BY),
# each with its share of the Thing's simple aggregates as thing.<label>. One engine: a row Thing, a partitioned
# Thing, a Thing over a join, a key with no rows, the NULL key, LIMIT, and what is refused.
#
#   thing-explain-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-explain-check.sh <aios binary>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
failed=0
check() { if [ "$2" = 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=1; fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/pass"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/token"
printf '[client]\nuser=app\nhost=127.0.0.1\npassword=%s\n' "$(cat "$W/pass")" > "$W/c.cnf"
P=$(free_port)
q() { mysql --defaults-extra-file="$W/c.cnf" -P "$P" -B -e "USE shop; $1" 2>&1; }   # with the header row
"$BIN" --root "$W/r" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$W/e.log" 2>&1 < /dev/null & PIDS+=($!)
for _ in $(seq 400); do mysql --defaults-extra-file="$W/c.cnf" -P "$P" -N -B -e "SELECT 1" 2>/dev/null | grep -qx 1 && break; sleep 0.05; done
mysql --defaults-extra-file="$W/c.cnf" -P "$P" -e "CREATE DATABASE shop" > /dev/null 2>&1

q "CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, wh VARCHAR(4), sku VARCHAR(8), qty INT, price DECIMAL(10,2));
   INSERT INTO lines VALUES (1, 100, 'a', 'T1', 2, 5.25), (2, 100, 'b', 'T2', 1, 10.00), (3, 200, 'a', 'T1', 5, 1.00),
                            (4, NULL, 'a', 'T3', 1, 1.00), (5, 300, 'a', 'T1', 0, 2.00);
   CREATE TABLE skus (sku VARCHAR(8) PRIMARY KEY, brand VARCHAR(8));
   INSERT INTO skus VALUES ('T1', 'acme'), ('T2', 'zen'), ('T3', 'acme')" > /dev/null
q "CREATE THING orders KEY orderno PARTITION BY orderno READS lines
   AS SELECT orderno, COUNT(*) AS n, SUM(qty * price) AS total, MAX(sku) AS top, CASE WHEN SUM(qty) > 2 THEN 'big' ELSE 'small' END AS size
   FROM lines WHERE qty > 0 GROUP BY orderno" > /dev/null
q "CREATE THING by_wh KEY wh PARTITION BY orderno READS lines AS SELECT wh, SUM(qty) * 2 AS doubled FROM lines GROUP BY wh" > /dev/null
q "CREATE THING by_brand KEY brand PARTITION BY brand READS lines, skus
   AS SELECT brand, SUM(l.qty) AS units FROM lines l INNER JOIN skus s ON s.sku = l.sku GROUP BY brand" > /dev/null

made=$(q "SHOW THINGS" | cut -f1 | grep -cxE 'orders|by_wh|by_brand')
[ "$made" = 3 ] || { echo "FAIL the three Things were not all made: $(q "SHOW THINGS" | cut -f1 | tr '\n' ' ')"; exit 1; }
r=$(q "EXPLAIN THING orders '100'")
hdr=$(echo "$r" | head -1)
echo "$hdr" | grep -q "thing.n" && echo "$hdr" | grep -q "thing.total" && echo "$hdr" | grep -q "thing.top" && ! echo "$hdr" | grep -q "thing.size"; rc=$?
check "a row Thing: its source columns, and a share column per simple aggregate (not the CASE) [$hdr]" $([ $rc = 0 ] && echo 1 || echo 0)
col() { echo "$r" | head -1 | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1; }
ids=$(echo "$r" | tail -n +2 | cut -f"$(col id)" | sort | tr '\n' ' ')
tot=$(echo "$r" | tail -n +2 | cut -f"$(col thing.total)" | paste -sd+ | bc)
cnt=$(echo "$r" | tail -n +2 | cut -f"$(col thing.n)" | paste -sd+ | bc)
thing=$(q "SHOW THING orders" | awk -F'\t' '$1=="100"{print $2" "$3}')
check "order 100 is lines 1 and 2, and their shares add up to the Thing's n and total [$ids| $cnt $tot vs $thing]" \
  $([ "$ids" = "1 2 " ] && [ "$cnt $tot" = "$(echo "$thing" | awk '{printf "%s %.2f", $1, $2}')" ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING orders '300'")
check "the Thing's WHERE holds: order 300's only line has qty 0, so no rows [$(echo "$r" | wc -l) lines]" $([ "$(echo "$r" | tail -n +2 | wc -l)" = 0 ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING orders NULL")
check "the NULL key is its own rows [$(echo "$r" | tail -n +2 | cut -f1 | tr '\n' ' ')]" $([ "$(echo "$r" | tail -n +2 | cut -f1 | tr '\n' ' ')" = "4 " ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING by_wh 'a'")
col() { echo "$r" | head -1 | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1; }
s=$(echo "$r" | tail -n +2 | cut -f"$(col thing.doubled)" | paste -sd+ | bc)
check "a partitioned Thing, SUM(x) * 2: the shares add up to its value [$s vs $(q "SHOW THING by_wh" | awk -F'\t' '$1=="a"{print $2}')]" \
  $([ "$s" = "$(q "SHOW THING by_wh" | awk -F'\t' '$1=="a"{print $2}')" ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING by_brand 'acme'")
col() { echo "$r" | head -1 | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1; }
check "a Thing over a join: the joined rows of the brand [$(echo "$r" | tail -n +2 | wc -l) rows, units $(echo "$r" | tail -n +2 | cut -f"$(col thing.units)" | paste -sd+ | bc)]" \
  $([ "$(echo "$r" | tail -n +2 | wc -l)" = 4 ] && [ "$(echo "$r" | tail -n +2 | cut -f"$(col thing.units)" | paste -sd+ | bc)" = 8 ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING by_brand 'acme' LIMIT 2")
check "LIMIT caps the rows [$(echo "$r" | tail -n +2 | wc -l)]" $([ "$(echo "$r" | tail -n +2 | wc -l)" = 2 ] && echo 1 || echo 0)
r=$(q "EXPLAIN THING nope '1'")
check "an unknown Thing is refused [$(echo "$r" | tail -1)]" $(echo "$r" | grep -q "no Thing kind named nope" && echo 1 || echo 0)
r=$(q "EXPLAIN THING orders")
check "a missing key is refused with the form [$(echo "$r" | tail -1)]" $(echo "$r" | grep -q "EXPLAIN THING <name> '<key>'" && echo 1 || echo 0)
r=$(q "EXPLAIN SELECT * FROM lines WHERE id = 1")
check "plain EXPLAIN still explains a query [$(echo "$r" | head -1 | cut -c1-60)]" $(echo "$r" | grep -qv "Thing" && ! echo "$r" | grep -qi error && echo 1 || echo 0)

[ "$failed" = 0 ] && echo "thing-explain: ok"
exit $failed
