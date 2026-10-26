#!/usr/bin/env bash
# Runs the grain scripts that need local servers: tests/grain/test_http_gzip.gr against tests/grain/net/gzip_server.py,
# tests/grain/test_mail.gr against tests/grain/net/smtp_sink.py (whose kept messages Python's email package then
# parses). Free loopback ports; the servers are stopped and their files removed afterwards.
#   scripts/grain-net-check.sh [path/to/grain]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
grain="${1:-$here/build-grain/grain}"
work="$(mktemp -d)"
server_pid=""
smtp_pid=""
cleanup() {
  # `|| true` throughout: under set -e, wait returning a stopped server's status (143) would end this trap there.
  for p in $server_pid $smtp_pid; do
    kill "$p" 2>/dev/null || true
    wait "$p" 2>/dev/null || true
  done
  rm -rf "$work"
}
trap cleanup EXIT
python3 -I "$here/tests/grain/net/gzip_server.py" "$work/port" &
server_pid=$!
for _ in $(seq 1 50); do [ -s "$work/port" ] && break; sleep 0.1; done
[ -s "$work/port" ] || { echo "the test HTTP server did not start"; exit 1; }
export GRAIN_TEST_HTTP_PORT="$(cat "$work/port")"
mkdir -p "$work/mail"
python3 -I "$here/tests/grain/net/smtp_sink.py" "$work/smtp_port" "$work/mail" &
smtp_pid=$!
for _ in $(seq 1 50); do [ -s "$work/smtp_port" ] && break; sleep 0.1; done
[ -s "$work/smtp_port" ] || { echo "the test SMTP server did not start"; exit 1; }
export GRAIN_TEST_SMTP_PORT="$(cat "$work/smtp_port")" GRAIN_TEST_SMTP_DIR="$work/mail"
rc=0
for t in "$here"/tests/grain/test_http_gzip.gr "$here"/tests/grain/test_mail.gr; do
  echo "== $(basename "$t")"
  "$grain" "$t" || rc=1
done
# Every message the sink kept, read by a real MIME parser: headers decode, parts and attachments come back whole.
python3 -I - "$work/mail" <<'PY' || rc=1
import email, email.policy, glob, json, sys
ok = True
def check(c, what):
    global ok
    print(("ok   " if c else "FAIL ") + what); ok = ok and c
msgs = {int(p.rsplit("/", 1)[1].split(".")[0]): json.load(open(p)) for p in glob.glob(sys.argv[1] + "/*.json")}
check(len(msgs) >= 4, "the sink kept the messages: %d" % len(msgs))
m1 = email.message_from_string(msgs[1]["data"], policy=email.policy.default)
check(m1["Subject"] == "Daily report" and m1["Bcc"] is None and m1["Message-ID"].endswith("@qbc.example>"), "message 1 headers")
check(m1.get_content().replace("\r\n", "\n") == "Hello.\n.leading dot line\nBye.\n", "message 1 body (CRLF on the wire): %r" % m1.get_content())
m2 = email.message_from_string(msgs[2]["data"], policy=email.policy.default)
check(m2["Subject"] == "Preise für Reifen ✓", "UTF-8 subject decodes: %r" % m2["Subject"])
check(m2["From"].addresses[0].display_name == "Ünïcode Feed", "UTF-8 display name decodes")
body = m2.get_body(preferencelist=("html", "plain"))
check(body.get_content_type() == "text/html" and "Grüße" in body.get_content(), "the html alternative")
check(m2.get_body(preferencelist=("plain",)).get_content().strip() == "Grüße", "the text alternative")
att = list(m2.iter_attachments())
check([a.get_filename() for a in att] == ["bericht-ü.csv", "logo.bin"], "attachment names: %r" % [a.get_filename() for a in att])
check(att[0].get_content() == "sku,qty\n1,2\n" and att[1].get_content() == bytes.fromhex("00ff7f80"), "attachment contents")
sys.exit(0 if ok else 1)
PY
exit $rc
