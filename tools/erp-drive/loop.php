<?php
mysqli_report(MYSQLI_REPORT_OFF); $c = new mysqli('127.0.0.1','root','','summitholding',(int)$argv[1]);
$q = preg_replace('/\)\s*SELECT DISTINCT/', ') SELECT SQL_NO_CACHE DISTINCT', file_get_contents($argv[2]), 1);
$end = microtime(true) + (float)$argv[3]; $n = 0; while (microtime(true) < $end) { $c->query($q)->fetch_all(); $n++; } echo "ran $n\n";
