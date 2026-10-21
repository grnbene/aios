<?php
// The concurrent-transaction rules, each driven over real connections (AIOS_TXN_CONCURRENT=1 on the server).
mysqli_report(MYSQLI_REPORT_OFF);
$P = (int)$argv[1];
$c = fn() => new mysqli("127.0.0.1", "root", "secret", "summitholding", $P);
$one = fn($m, $q) => $m->query($q)->fetch_row()[0];
$ok = 0; $bad = 0;
$check = function ($what, $pass, $detail = '') use (&$ok, &$bad) { $pass ? $ok++ : $bad++; printf("  %-4s %s%s\n", $pass ? 'ok' : 'FAIL', $what, $detail === '' ? '' : "  ($detail)"); };
// starts $q on $m and reports whether it finished within $ms
$start = function ($m, $q) { $m->query($q, MYSQLI_ASYNC); return hrtime(true); };
$done_within = function ($m, $ms) { $r = [$m]; $e = [$m]; $x = [$m]; return mysqli_poll($r, $e, $x, 0, $ms * 1000) > 0; };
$finish = function ($m) { $r = $m->reap_async_query(); return $r === false ? $m->errno : 0; };

echo "1. two transactions take stock from the same bin: neither waits\n";
$a = $c(); $b = $c();
$a->query("START TRANSACTION"); $b->query("START TRANSACTION");
$a->query("update locstock set quantity = quantity - 1 where stockid = 'SHAMPOO' and bin = 'A1' and loccode = 'MAIN'");
$t = $start($b, "update locstock set quantity = quantity - 2 where stockid = 'SHAMPOO' and bin = 'A1' and loccode = 'MAIN'");
$check("B's relative update went straight through", $done_within($b, 300)); $finish($b);
$a->query("ROLLBACK"); $b->query("COMMIT");
$check("A's 1 back, B's 2 kept", $one($a, "select quantity from locstock where stockid = 'SHAMPOO'") == 998, $one($a, "select quantity from locstock where stockid = 'SHAMPOO'"));

echo "2. two transactions set the same row absolutely: the second waits for the first\n";
$a->query("START TRANSACTION"); $b->query("START TRANSACTION");
$a->query("update debtorsmaster set paymentterms = '7' where debtorno = 'C1'");
$start($b, "update debtorsmaster set paymentterms = '14' where debtorno = 'C1'");
$check("B waits while A holds the row", !$done_within($b, 400));
$a->query("COMMIT");
$check("B goes once A commits", $done_within($b, 2000)); $finish($b);
$b->query("COMMIT");
$check("B's value is last", $one($a, "select paymentterms from debtorsmaster where debtorno = 'C1'") === '14');

echo "3. a deadlock: one of the two is told 1213 and rolled back, the other finishes\n";
$a->query("START TRANSACTION"); $b->query("START TRANSACTION");
$a->query("update debtorsmaster set name = 'A1' where debtorno = 'C1'");
$b->query("update debtorsmaster set name = 'B2' where debtorno = 'C2'");
$start($a, "update debtorsmaster set name = 'A2' where debtorno = 'C2'");   // A waits for B
usleep(200000);
$b->query("update debtorsmaster set name = 'B1' where debtorno = 'C1'");    // B would wait for A: a cycle
$berr = $b->errno;
$check("B got 1213", $berr == 1213, "errno $berr: " . $b->error);
$check("A then went through", $done_within($a, 2000)); $aerr = $finish($a);
$check("A's statement succeeded", $aerr == 0, "errno $aerr");
$a->query("COMMIT");
$check("A's writes stand, B's were rolled back", $one($a, "select group_concat(name order by debtorno) from debtorsmaster where debtorno in ('C1','C2')") === 'A1,A2', $one($a, "select group_concat(name order by debtorno) from debtorsmaster where debtorno in ('C1','C2')"));

echo "4. SELECT ... FOR UPDATE: a second one on the same row waits\n";
$a->query("insert into debtortrans (id, transno, type, debtorno, alloc) values (1, 1, 10, 'C1', 0)");
$a->query("START TRANSACTION"); $b->query("START TRANSACTION");
$a->query("select alloc from debtortrans where id = 1 for update");
$start($b, "select alloc from debtortrans where id = 1 for update");
$check("B's FOR UPDATE waits", !$done_within($b, 400));
$a->query("update debtortrans set alloc = alloc + 10 where id = 1"); $a->query("COMMIT");
$check("B's goes after A commits", $done_within($b, 2000));
$r = $b->reap_async_query(); $v = $r ? $r->fetch_row()[0] : null;
$check("B reads A's committed allocation", $v == 10, "read $v");
$b->query("COMMIT");

echo "5. claiming an order: the second claim waits, and goes ahead when the first rolls back\n";
$a->query("START TRANSACTION"); $b->query("START TRANSACTION");
$a->query("update salesorders set invoiceno = if(coalesce(invoiceno, 0) = 0, 5001, invoiceno) where orderno = '7'");
$start($b, "update salesorders set invoiceno = if(coalesce(invoiceno, 0) = 0, 5002, invoiceno) where orderno = '7'");
$check("B's claim waits for A", !$done_within($b, 400));
$a->query("ROLLBACK");
$check("B's claim goes after A rolls back", $done_within($b, 2000)); $finish($b);
$check("B holds the order", $one($b, "select invoiceno from salesorders where orderno = 7") == 5002);
$b->query("COMMIT");

echo "6. an autocommit write waits for a transaction holding the row, a relative one does not\n";
$a->query("START TRANSACTION");
$a->query("update locstock set quantity = 500 where stockid = 'DRYER'");
$start($b, "update locstock set quantity = quantity - 1 where stockid = 'DRYER'");
$check("B (autocommit, relative) waits on A's absolute write", !$done_within($b, 400));
$a->query("COMMIT");
$check("B goes after", $done_within($b, 2000)); $finish($b);
$check("DRYER = 499", $one($b, "select quantity from locstock where stockid = 'DRYER'") == 499);
$a->query("START TRANSACTION");
$a->query("update locstock set quantity = quantity - 1 where stockid = 'DRYER'");
$start($b, "update locstock set quantity = quantity - 1 where stockid = 'DRYER'");
$check("B (autocommit, relative) passes A's relative write", $done_within($b, 300)); $finish($b);
$a->query("ROLLBACK");
$check("DRYER = 498 (B's 1 kept, A's 1 back)", $one($b, "select quantity from locstock where stockid = 'DRYER'") == 498);
printf("%d ok, %d failed\n", $ok, $bad);
