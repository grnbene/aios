#!/usr/bin/env bash
# What tests/grain/test_logging.gr writes, checked from outside (a grain script cannot read its own output): each
# level's text line, the JSON line, nothing below the threshold, and eprint on stderr only.
#   scripts/grain-logging-check.sh [path/to/grain]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
grain="${1:-$here/build-grain/grain}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$grain" "$here/tests/grain/test_logging.gr" > "$work/out" 2> "$work/err"
python3 - "$work/out" "$work/err" <<'PY'
import json, re, sys
out = open(sys.argv[1]).read().splitlines()
err = open(sys.argv[2]).read().splitlines()
fails = []
def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok: fails.append(what)
ts = r"\[\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\]"
check(any(re.fullmatch(ts + r' \[INFO\] Application starting \{"port":8080, "env":"prod"\}', l) for l in out), "text line: time, level, message, extra map")
for lvl, msg in [("DEBUG", "Cache initialized"), ("WARN", "Slow query detected"), ("ERROR", "Failed to connect")]:
    check(any(re.fullmatch(ts + r" \[" + lvl + r"\] " + msg + r".*", l) for l in out), lvl + " line")
js = [l for l in out if l.startswith("{")]
check(len(js) == 1, "one JSON line")
if js:
    o = json.loads(js[0])
    check(o.get("level") == "INFO" and o.get("msg") == "JSON structured log" and o.get("status") == 200 and o.get("user") == "admin", "JSON line fields: " + js[0])
check(not any("Silent" in l for l in out + err), "below the threshold: nothing written")
check(any(l.endswith("[ERROR] Emitted error") for l in out), "at the threshold: written")
check(err == ["Testing stderr output from eprint"], "eprint: stderr only")
check(not any("eprint" in l for l in out), "eprint: not on stdout")
check(out[-1] == "PASS: test_logging", "the script ran to the end")
sys.exit(1 if fails else 0)
PY
