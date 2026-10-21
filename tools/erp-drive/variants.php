<?php
mysqli_report(MYSQLI_REPORT_OFF); $c = new mysqli('127.0.0.1','root','','summitholding',(int)$argv[1]);
$base = file_get_contents($argv[2]);
$variants = [
 'as sent' => $base,
 'no DISTINCT' => preg_replace('/SELECT DISTINCT\s+salesorderdetails/','SELECT salesorderdetails',$base,1),
 'narrow cols' => preg_replace('/\)\s*SELECT DISTINCT.*?FROM\s+salesorderdetails/s',") SELECT DISTINCT salesorders.orderno, debtorsmaster.debtorno, sum(qty) as sumqty FROM salesorderdetails",$base,1),
 'no CTE anti-join' => str_replace(['LEFT JOIN dispatched ON dispatched.orderno = salesorders.orderno','AND dispatched.orderno is NULL'],['',''],$base),
 'no stockmoves' => preg_replace('/LEFT JOIN stockmoves ON[^\n]*\n/','',$base),
 'no ORDER BY' => preg_replace('/ORDER BY salesorders.date_created desc/','',$base),
];
foreach ($variants as $n => $q) {
  $q = preg_replace('/\)\s*SELECT DISTINCT/', ') SELECT SQL_NO_CACHE DISTINCT', $q, 1);
  $q = preg_replace('/\)\s*SELECT (?!SQL_NO_CACHE)/', ') SELECT SQL_NO_CACHE ', $q, 1);
  $best = 1e9; $rows = 0; $bytes = 0;
  for ($i = 0; $i < 3; $i++) { $t = hrtime(true); $r = @$c->query($q); if (!$r) { echo "$n ERR ", $c->error, "\n"; continue 2; } $all = $r->fetch_all(); $best = min($best, (hrtime(true)-$t)/1e6); $rows = count($all); $bytes = strlen(json_encode($all)); }
  printf("%-18s %8.1f ms  rows=%d  cols=%d  ~%.1f MB\n", $n, $best, $rows, $r->field_count, $bytes/1e6);
}
