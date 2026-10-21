<?php
// Rollback must take back the transaction's own changes and nothing anyone else did.
$P = (int)$argv[1];
$c = fn() => new mysqli("127.0.0.1", "root", "secret", "summitholding", $P);
$one = fn($m, $q) => $m->query($q)->fetch_row()[0];
$t = $c(); $b = $c();
$ok = 0; $bad = 0;
$check = function ($what, $got, $want) use (&$ok, &$bad) { $pass = (string)$got === (string)$want; $pass ? $ok++ : $bad++; printf("  %-4s %-62s got %s, want %s\n", $pass ? 'ok' : 'FAIL', $what, $got, $want); };

echo "1. another session's sale while an invoice is open, then the invoice rolls back\n";
$t->query("START TRANSACTION");
$t->query("update locstock set quantity = quantity - 1 where stockid = 'SHAMPOO'");
$t->query("update locstock set quantity = quantity - 2 where stockid = 'DRYER'");
$t->query("insert into gltrans (type, typeno, amount) values (10, 1, 1)");
$b->query("update locstock set quantity = quantity - 5 where stockid = 'DRYER'");          // B: same row T changed
$b->query("insert into gltrans (type, typeno, amount) values (12, 77, 5)");               // B: appended after T's row
$b->query("update debtorsmaster set name = 'Salon One Ltd' where debtorno = 'C1'");
$t->query("update debtorsmaster set paymentterms = '7' where debtorno = 'C1'");           // T: another column of B's row
$t->query("delete from salesorderdetails where orderno = 40 and orderlineno = 0");
$t->query("ROLLBACK");
$check("DRYER: B's sale of 5 stays, T's 2 come back", $one($b, "select quantity from locstock where stockid='DRYER'"), 95);
$check("SHAMPOO: T's 1 comes back", $one($b, "select quantity from locstock where stockid='SHAMPOO'"), 1000);
$check("B's gltrans row stays", $one($b, "select count(*) from gltrans where typeno = 77"), 1);
$check("T's gltrans row goes", $one($b, "select count(*) from gltrans where typeno = 1"), 0);
$check("B's rename stays", $one($b, "select name from debtorsmaster where debtorno='C1'"), 'Salon One Ltd');
$check("T's terms change goes", $one($b, "select paymentterms from debtorsmaster where debtorno='C1'"), '30');
$check("T's deleted order line comes back", $one($b, "select count(*) from salesorderdetails where orderno = 40"), 3);

echo "2. a savepoint\n";
$t->query("START TRANSACTION");
$t->query("insert into gltrans (type, typeno, amount) values (10, 2, 1)");
$t->query("SAVEPOINT a");
$t->query("insert into gltrans (type, typeno, amount) values (10, 3, 1)");
$t->query("update locstock set quantity = quantity + 100 where stockid = 'SHAMPOO'");
$t->query("ROLLBACK TO SAVEPOINT a");
$t->query("COMMIT");
$check("kept before the savepoint", $one($b, "select count(*) from gltrans where typeno = 2"), 1);
$check("taken back after it", $one($b, "select count(*) from gltrans where typeno = 3"), 0);
$check("SHAMPOO unchanged", $one($b, "select quantity from locstock where stockid='SHAMPOO'"), 1000);

echo "3. a table with no primary key\n";
$b->query("create table nokey (a int, b varchar(10))");
$b->query("insert into nokey values (1, 'x'), (1, 'x'), (2, 'y')");
$t->query("START TRANSACTION");
$t->query("insert into nokey values (1, 'x')");
$t->query("update nokey set b = 'z' where a = 2");
$t->query("delete from nokey where a = 1 limit 1");
$t->query("ROLLBACK");
$check("rows back as they were", $one($b, "select concat(count(*), ':', sum(a = 1), ':', max(b)) from nokey"), '3:2:y');

echo "4. an invoice's ids are not reused after a rollback, and the next one commits\n";
$t->query("START TRANSACTION"); $t->query("insert into debtortrans (transno, type, debtorno) values (900, 10, 'C1')"); $rid = $t->insert_id; $t->query("ROLLBACK");
$t->query("insert into debtortrans (transno, type, debtorno) values (901, 10, 'C1')");
$check("rolled-back row gone, next row there", $one($b, "select group_concat(transno) from debtortrans"), '901');
printf("%d ok, %d failed\n", $ok, $bad);
