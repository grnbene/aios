#!/usr/bin/env bash
# lockcost.sh PORT RTT_US: 8 workers invoicing 40 orders with a network round trip per statement, while a POS
# terminal rings up sales (each its own small transaction). How long does a sale wait?
P=$1; RTT=$2
t0=$(date +%s.%N)
for w in 0 1 2 3 4 5 6 7; do (for o in $(seq $((w*5+1)) $((w*5+5))); do php drive.php $P $o TAG=w$w RTT_US=$RTT; done) > /tmp/lc.$P.$w 2>&1 & done
php -r '$m = new mysqli("127.0.0.1", "root", "secret", "summitholding", (int)$argv[1]); $rtt = (int)$argv[2]; $ts = [];
  usleep(100000);
  for ($i = 0; $i < 40; $i++) { $t = hrtime(true);
    foreach (["START TRANSACTION", "update locstock set quantity = quantity - 1 where stockid = \"SHAMPOO\" and bin = \"A1\"", "insert into gltrans (type, typeno, account, amount) values (12, " . (7000 + $i) . ", \"POS\", 1)", "COMMIT"] as $q) { usleep($rtt); $m->query($q); }
    $ts[] = (hrtime(true) - $t) / 1e6; usleep(20000); }
  sort($ts); printf("POS sale (4 statements): median %.0f ms, p90 %.0f ms, max %.0f ms\n", $ts[20], $ts[36], $ts[39]);' $P $RTT &
wait
t1=$(date +%s.%N)
printf "invoices: %d in %.2f s; per invoice median %s ms (alone it takes about 160 x RTT + engine time)\n" $(cat /tmp/lc.$P.* | grep -c '"success":true') $(echo "$t1 - $t0" | bc) $(cat /tmp/lc.$P.* | grep -o '([0-9]* ms' | tr -d '(ms ' | sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)+1]}')
