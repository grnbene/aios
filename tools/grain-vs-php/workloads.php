<?php
// The same nine workloads as workloads.gr, written the way PHP is usually written. Run by run.sh.
function t($name, $f) { $a = hrtime(true); $r = $f(); echo $name, "\t", round((hrtime(true) - $a) / 1e6), "\t", $r, "\n"; }
function fib($n) { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
t("fib27", fn() => fib(27));
t("loop10M", function () { $s = 0; for ($i = 0; $i < 10000000; $i++) $s += $i % 7; return $s; });
t("concat1M", function () { $p = []; for ($i = 0; $i < 1000000; $i++) $p[] = "x" . $i; return strlen(implode(",", $p)); });
t("records200k", function () {
  $rows = []; for ($i = 0; $i < 200000; $i++) $rows[] = ["id" => $i, "store" => "S" . ($i % 50), "qty" => $i % 13];
  $tot = []; foreach ($rows as $r) $tot[$r["store"]] = ($tot[$r["store"]] ?? 0) + $r["qty"]; return count($tot); });
t("sort200k", function () { $xs = []; $x = 12345; for ($i = 0; $i < 200000; $i++) { $x = ($x * 1103515245 + 12345) % 2147483648; $xs[] = $x; } sort($xs); return $xs[0]; });
t("json50k", function () {
  $rows = []; for ($i = 0; $i < 50000; $i++) $rows[] = ["id" => $i, "name" => "item " . $i, "price" => 1.5 * $i, "tags" => ["a", "b"]];
  $txt = json_encode($rows); $back = json_decode($txt, true); return count($back); });
t("regex100k", function () {
  $lines = []; for ($i = 0; $i < 100000; $i++) $lines[] = "order-" . $i . " qty=" . ($i % 9) . " ok";
  $n = 0; foreach ($lines as $l) $n += strlen(preg_replace('/qty=([0-9]+)/', 'Q$1', $l)); return $n; });
t("split5MB", function () {
  $lines = []; for ($i = 0; $i < 100000; $i++) $lines[] = " " . $i . ", Store " . ($i % 50) . " , 12.50 , yes ";
  $text = implode("\n", $lines); $n = 0;
  foreach (explode("\n", $text) as $l) { $f = explode(",", $l); $n += strlen(trim($f[1])); } return $n; });
t("html20k", function () {
  $out = "<table>"; for ($i = 0; $i < 20000; $i++) $out .= "<tr><td>" . htmlspecialchars((string)$i) . "</td><td>" . htmlspecialchars("Bay <" . ($i % 9) . ">") . "</td><td>ok</td></tr>";
  return strlen($out . "</table>"); });
