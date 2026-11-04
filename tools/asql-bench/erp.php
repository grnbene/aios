<?php
// The three ERP reports on the ASQL measurement server (port 3398, summitholding): one warm-up run, 5 timed runs each.
$db = mysqli_init(); $db->real_connect('127.0.0.1', 'root', trim(file_get_contents(__DIR__ . '/pw')), 'summitholding', 3398);
$out = [];
foreach ([1 => 'thisoverlast', 2 => 'nopurchase', 3 => 'brandbylocation'] as $i => $name) {
  $sql = file_get_contents(__DIR__ . "/erp/asql$i.sql");
  $db->query($sql) or die($db->error);
  $ts = [];
  for ($k = 0; $k < 5; $k++) { $t = hrtime(true); $r = $db->query($sql); $rows = $r->fetch_all(MYSQLI_NUM); $ts[] = (hrtime(true) - $t) / 1e6; }
  sort($ts);
  $out[$name] = ['median_ms' => $ts[2], 'min_ms' => $ts[0], 'max_ms' => $ts[4], 'rows' => count($rows), 'answer' => $rows];
}
echo json_encode($out), "\n";
