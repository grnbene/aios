<?php
mysqli_report(MYSQLI_REPORT_OFF);
$A = new mysqli('127.0.0.1','root','','summitholding',13402); $B = new mysqli('127.0.0.1','root','','summitholding',13401);
$same = 0; $diff = 0;
foreach (glob($argv[1] . '/stmt-*.sql') as $f) {
  $q = file_get_contents($f);
  if (!preg_match('/^\s*(SELECT|WITH)/i', $q)) continue;
  $ra = $A->query($q); $rb = $B->query($q);
  $ja = $ra ? json_encode($ra->fetch_all()) : 'ERR ' . $A->error; $jb = $rb ? json_encode($rb->fetch_all()) : 'ERR ' . $B->error;
  if ($ja === $jb) $same++; else { $diff++; echo "DIFF ", basename($f), ": ", substr(preg_replace('/\s+/', ' ', $q), 0, 140), "\n"; }
}
echo "identical $same, different $diff\n";
