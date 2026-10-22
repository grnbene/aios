#!/usr/bin/env bash
# Drives aios-webserver with a site written in grain (scripts/grain-web): static files, routes, JSON, forms,
# redirects, state in files, native fn, and the ways a handler can fail or be abused. Needs curl.
#   grain-web-check.sh <aios-webserver>
set -u
BIN=${1:?usage: grain-web-check.sh <aios-webserver>}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$WORK"' EXIT
cp -r "$HERE/grain-web/." "$WORK/"
mkdir -p "$WORK/data"
PORT=$((20000 + RANDOM % 20000))
UIPORT=$((PORT + 1))
LIVEPORT=$((PORT + 2))
mkdir -p "$WORK/live"
cp "$HERE/../grain/tests/web/live.gr" "$WORK/live/live.gr"
mkdir -p "$WORK/ui"
cp "$HERE/../grain/tests/web/counter.gr" "$WORK/ui/counter.gr"
echo "static next to the app" > "$WORK/ui/readme.txt"
cat > "$WORK/web.conf" <<CONF
vhost localhost {
  listen $PORT loopback
  root $WORK
  index index.html
  grain app.gr
  grain_native safe
  grain_steps 5000000
  grain_max_body 1000
  max_body_size 20000
}
vhost ui {
  listen $UIPORT loopback
  root $WORK/ui
  index none.html
  grain_ui counter.gr
}
vhost live {
  listen $LIVEPORT loopback
  root $WORK/live
  index none.html
  grain_ui live.gr
}
CONF
"$BIN" "$WORK/web.conf" > "$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done

U=http://127.0.0.1:$PORT
FAILED=0
check() {   # check <name> <expected> <actual>
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected [$2], got [$3]"; FAILED=1; fi
}
body() { curl -s -m 15 "$@"; }
code() { curl -s -m 15 -o /dev/null -w '%{http_code}' "$@"; }
hdr()  { local name=$1; shift; curl -s -m 15 -D - -o /dev/null "$@" | tr -d '\r' | grep -ai "^$name:" | head -1 | cut -d' ' -f2-; }

check "static file"              "<h1>static index</h1>"  "$(body $U/)"
check "static css type"          "text/css"               "$(hdr content-type $U/style.css)"
check "route with query"         "hello grain"            "$(body "$U/hello?name=grain")"
check "route default"            "hello world"            "$(body $U/hello)"
check "json response"            '{"method":"POST","query":{"x":"z"},"form":{"a":"1","b":"two words"},"len":17,"ua":"t"}' \
                                 "$(body -X POST -d 'a=1&b=two%20words' -A t "$U/echo?x=y&x=z")"
check "json content-type"        "application/json"       "$(hdr content-type "$U/echo")"
check "native fn in a handler"   "832040"                 "$(body "$U/fib?n=30")"
check "redirect status"          "302"                    "$(code $U/old)"
check "redirect location"        "/hello"                 "$(hdr location $U/old)"
check "state kept in a file"     "visits: 1 visits: 2"    "$(body $U/count) $(body $U/count)"
check "custom header"            "3"                      "$(hdr x-count $U/count)"
check "a path cannot leave root" "500"                    "$(code $U/escape)"
check "an error is a 500"        "500"                    "$(code $U/boom)"
check "the error stays private"  "internal server error"  "$(body $U/boom)"
check "a runaway loop stops"     "500"                    "$(code $U/loop)"
check "a header cannot split"    ""                       "$(hdr set-cookie $U/crlf)"
check "nil is 204"               "204"                    "$(code $U/none)"
check "a list body is JSON"      "[1,2,{\"a\":true}]"     "$(body $U/list)"
check "list status"              "201"                    "$(code $U/list)"
check "bytes body"               "486900ff"               "$(body $U/bytes | od -An -tx1 -v | tr -d ' \n')"
check "the script is not served" "404"                    "$(code $U/app.gr)"
check "an unrouted path"         "no route for /nothing"  "$(body $U/nothing)"
check "HEAD sends no body"       "0"                      "$(curl -s -I -m 15 $U/hello | grep -c 'hello world')"
check "HEAD status"              "200"                    "$(code -I $U/hello)"
check "a small body is buffered"      '{"streamed":false,"bytes":5,"body_is_nil":false}' "$(body --data-binary hello $U/upload)"
check "a big body is streamed"        '{"streamed":true,"bytes":5000,"body_is_nil":true}' "$(body --data-binary @<(head -c 5000 /dev/zero | tr '\0' x) $U/upload)"
check "a chunked body is streamed"    '{"streamed":true,"bytes":3000,"body_is_nil":true}' "$(body -H 'Transfer-Encoding: chunked' --data-binary @<(head -c 3000 /dev/zero | tr '\0' x) $U/upload)"
check "a small chunked body is buffered" '{"streamed":false,"bytes":5,"body_is_nil":false}' "$(body -H 'Transfer-Encoding: chunked' --data-binary hello $U/upload)"
check "past max_body_size is refused"  "413"                  "$(code --data-binary @<(head -c 30000 /dev/zero | tr '\0' x) $U/upload)"
check "past it, chunked, is refused"   "413"                  "$(code -H 'Transfer-Encoding: chunked' --data-binary @<(head -c 30000 /dev/zero | tr '\0' x) $U/upload)"
check "gzip when asked"               "gzip"                  "$(hdr content-encoding -H 'Accept-Encoding: gzip' $U/big)"
check "gzip body is the same"         "5000"                  "$(curl -s -m 15 --compressed $U/big | wc -c | tr -d ' ')"
check "no gzip when not asked"        ""                      "$(hdr content-encoding $U/big)"
check "small bodies are not gzipped"  ""                      "$(hdr content-encoding -H 'Accept-Encoding: gzip' $U/hello)"
check "not yet defined"               "404"                   "$(code $U/reloaded)"
sleep 1.1
sed -i 's|  if p == "/old"|  if p == "/reloaded" { return text("v2") }\n  if p == "/old"|' "$WORK/app.gr"
check "an edited script is picked up" "v2"                    "$(body $U/reloaded)"
check "the server still works"   "hello world"            "$(body $U/hello)"

# a ui.pane program as a web page (grain_ui): the same protocol test as `grain serve`
check "grain_ui serves static files too"  "static next to the app" "$(body http://127.0.0.1:$UIPORT/readme.txt | tr -d '\n')"
check "grain_ui does not serve the source" "404"                 "$(code http://127.0.0.1:$UIPORT/counter.gr)"
if command -v python3 >/dev/null; then
  if GRAIN_WEB_PORT=$UIPORT python3 "$HERE/../grain/tests/web/web_ui_test.py" > "$WORK/ui.out" 2>&1; then echo "ok   grain_ui: $(grep -c '^ok' "$WORK/ui.out") protocol checks"
  else echo "FAIL grain_ui:"; cat "$WORK/ui.out"; FAILED=1; fi
fi

# live data through the server: pushed updates, shared state, nothing asked for
if command -v python3 >/dev/null; then
  if GRAIN_WEB_PORT=$LIVEPORT python3 "$HERE/../grain/tests/web/live_test.py" > "$WORK/live.out" 2>&1; then echo "ok   live data: $(grep -c '^ok' "$WORK/live.out") checks (pushed over a held connection)"
  else echo "FAIL live data:"; cat "$WORK/live.out"; FAILED=1; fi
fi

exit $FAILED
