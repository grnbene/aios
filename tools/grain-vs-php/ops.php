<?php
// Single operations a million times each, against ops.gr.
function t($n, $f) { $a = hrtime(true); $f(); echo $n, "\t", round((hrtime(true) - $a) / 1e6), "\t-\n"; }
$N = 1000000;
t("empty loop", function () use ($N) { for ($i = 0; $i < $N; $i++) {} });
t("str(i)", function () use ($N) { for ($i = 0; $i < $N; $i++) { $s = (string)$i; } });
t("len(x) native call", function () use ($N) { $x = "abc"; for ($i = 0; $i < $N; $i++) { $n = strlen($x); } });
t("\"x\" + s", function () use ($N) { $s = "123456"; for ($i = 0; $i < $N; $i++) { $y = "x" . $s; } });
t("push(list, 1)", function () use ($N) { $l = []; for ($i = 0; $i < $N; $i++) { $l[] = 1; } });
t("push(list, str)", function () use ($N) { $l = []; $s = "abc"; for ($i = 0; $i < $N; $i++) { $l[] = $s; } });
$parts = []; for ($i = 0; $i < $N; $i++) $parts[] = "x" . $i;
t("join 1M", function () use ($parts) { $j = implode(",", $parts); });
