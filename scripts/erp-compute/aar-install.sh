#!/usr/bin/env bash
# Install the aios compute service and the ERP's PHP bridge on aar, idempotently.
# Run from arch:  bash aar-install.sh
# reports.php is not shipped: aar's copy is newer than the local tree, so the
# installer takes aar's own file, applies the bridge to it (idempotently), and puts it back.
#
# What it does, in order:
#   1. copies the headless static aios binary to /opt/aios/bin/aios
#   2. creates the `aios` system user and hands it /opt/aios/root
#   3. installs and starts aios-compute.service (loopback only, port 7311)
#   4. copies the four PHP files into /var/www/v8/server and reloads php-fpm
#   5. proves it: the service answers ping from PHP on aar, and solves a route
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=/repos/mcp-tools/aios/build-static-headless/aios
SRV=/repos/v8/server
HOST=aar

[ -x "$BIN" ] || { echo "no static binary at $BIN"; exit 1; }
file "$BIN" | grep -q "statically linked" || { echo "$BIN is not statically linked"; exit 1; }

echo "== 1/5 binary"
scp -q "$BIN" $HOST:/opt/aios/bin/aios.new
ssh $HOST 'chmod 755 /opt/aios/bin/aios.new && mv -f /opt/aios/bin/aios.new /opt/aios/bin/aios && /opt/aios/bin/aios --help >/dev/null 2>&1 && echo "   binary runs on aar"'

echo "== 2/5 user + root"
ssh $HOST 'id aios >/dev/null 2>&1 || useradd -r -s /sbin/nologin -d /opt/aios aios; chown -R aios:aios /opt/aios/root; echo "   user aios ok"'

echo "== 3/5 service"
scp -q "$HERE/aios-compute.service" $HOST:/etc/systemd/system/aios-compute.service
ssh $HOST 'systemctl daemon-reload && systemctl enable aios-compute.service >/dev/null 2>&1; before=$(systemctl show -p ExecMainStartTimestamp --value aios-compute.service); systemctl restart aios-compute.service; sleep 1.5; after=$(systemctl show -p ExecMainStartTimestamp --value aios-compute.service); echo "   start: ${before:-never} -> $after"; systemctl is-active aios-compute.service; ss -ltn | grep ":7311 " || { echo "   NOT LISTENING"; journalctl -u aios-compute.service -n 20 --no-pager; exit 1; }'

echo "== 4/5 php bridge"
scp -q "$SRV/catalog/controller/apir/oms.php" $HOST:/var/www/v8/server/catalog/controller/apir/oms.php
scp -q "$SRV/catalog/controller/apir/dispatch.php" $HOST:/var/www/v8/server/catalog/controller/apir/dispatch.php
scp -q "$SRV/system/library/aioscompute.php" $HOST:/var/www/v8/server/system/library/aioscompute.php
scp -q "$SRV/config_aios.php" $HOST:/var/www/v8/server/config_aios.php
# reports.php: patch aar's live copy in place rather than replace it.
WORK=$(mktemp -d); scp -q $HOST:/var/www/v8/server/catalog/controller/apir/reports.php "$WORK/reports.php"
python3 - "$WORK/reports.php" <<'PY2'
import sys
p = sys.argv[1]; s = open(p).read()
if 'aios.route_improve' in s:
    print('   reports.php already carries the bridge'); sys.exit(0)
anchor = ("        if (empty($stops)) {\n"
          "            return $this->response->setJsonOutput(array('success' => true, 'optimized' => array(), 'total_distance_miles' => 0));\n"
          "        }\n")
assert s.count(anchor) == 1, 'sales_route_optimize anchor not found exactly once'
hook = anchor + """
        // The aios compute service runs this same seed-and-improve as a kernel
        // and answers in milliseconds where PHP takes seconds at a day's worth
        // of stops. Its order is the one the code below would reach; when it
        // cannot answer in time, the code below reaches it.
        $aiosRoute = AiosCompute::routeImprove($start, $endMode, $end, array_values($stops));
        if (is_array($aiosRoute) && $aiosRoute['status'] === 'done') {
            $given     = array_values($stops);
            $optimized = array();
            foreach ($aiosRoute['order'] as $idx) {
                if (isset($given[(int) $idx])) {
                    $optimized[] = $given[(int) $idx];
                }
            }
            if (count($optimized) === count($given)) {
                return $this->response->setJsonOutput(array(
                    'success'              => true,
                    'optimized'            => $optimized,
                    'total_distance_miles' => round($aiosRoute['total_distance_miles'], 1),
                    'engine'               => 'aios.route_improve',
                    'aios'                 => array('evaluated' => $aiosRoute['evaluated'], 'passes' => $aiosRoute['passes'], 'kernel_ms' => $aiosRoute['elapsed_ms'], 'device' => $aiosRoute['device']),
                ));
            }
        }
"""
s = s.replace(anchor, hook, 1)
s = s.replace("""        $this->response->setJsonOutput(array(
            'success'              => true,
            'optimized'            => $optimized,
            'total_distance_miles' => round($total_distance, 1),
        ));""", """        $this->response->setJsonOutput(array(
            'success'              => true,
            'optimized'            => $optimized,
            'total_distance_miles' => round($total_distance, 1),
            'engine'               => 'php.salesRouteImprove',
        ));""", 1)
open(p, 'w').write(s); print('   reports.php bridged')
PY2
php -l "$WORK/reports.php" >/dev/null
scp -q "$WORK/reports.php" $HOST:/var/www/v8/server/catalog/controller/apir/reports.php; rm -rf "$WORK"
ssh $HOST 'cd /var/www/v8/server && for f in catalog/controller/apir/oms.php catalog/controller/apir/dispatch.php catalog/controller/apir/reports.php system/library/aioscompute.php config_aios.php; do php -l $f >/dev/null || exit 1; done; chmod 644 system/library/aioscompute.php config_aios.php; systemctl reload php-fpm; echo "   php ok, php-fpm reloaded"'

echo "== 5/5 proof from aar"
ssh $HOST 'cd /var/www/v8/server && php -r '"'"'require "system/library/aioscompute.php"; $p = AiosCompute::ping(); if (!$p) { echo "ping failed: ", AiosCompute::lastError(), "\n"; exit(1); } echo "   ping: kernel ", $p["kernel"], " lanes ", $p["lanes"], "\n"; $s=["id"=>"-1","lat"=>42.928455,"lng"=>-78.821427]; $st=[]; for($i=0;$i<12;$i++) $st[]=["id"=>"T".$i,"lat"=>42.80+0.021*$i*(($i%3)+1)/2,"lng"=>-78.95+0.037*$i]; $t=microtime(true); $r=AiosCompute::tspExact($s,$st,["deadline_ms"=>20000]); printf("   12-stop exact: %s, %.3f mi, evaluated %d, pruned %d, kernel %.1f ms, round trip %.1f ms, lanes %d\n", $r["status"], $r["distance_miles"], $r["evaluated"], $r["pruned"], $r["elapsed_ms"], (microtime(true)-$t)*1000, $r["lanes"]);'"'"''
echo "== done"
