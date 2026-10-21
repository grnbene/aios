#!/usr/bin/env bash
# up.sh PORT: a fresh scratch engine with the ERP schema loaded
P=${1:-33500}; R=${INV_DB:-/tmp}/invdb-$P; [ -f $R/pid ] && kill $(cat $R/pid) 2>/dev/null && sleep 1; rm -rf $R; mkdir -p $R/data; echo secret > $R/pass
${AIOS_SERVER:-aios-server} --port $P --bind 127.0.0.1 --root $R/data --user root --pass-file $R/pass > $R/log 2>&1 &
echo $! > $R/pid
for i in $(seq 1 100); do ${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -e "SELECT 1" >/dev/null 2>&1 && break; sleep 0.1; done
Q(){ ${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -d summitholding -e "$1"; }
${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -e "CREATE DATABASE summitholding" >/dev/null
while IFS= read -r line; do [ -n "$line" ] && Q "$line" 2>&1 | grep -i error; done < <(grep -v '^--' $(dirname "$0")/schema.sql $(dirname "$0")/extra.sql $(dirname "$0")/seed-base.sql $(dirname "$0")/orders.sql 2>/dev/null | sed 's/^[^:]*://')
echo "engine on $P"
