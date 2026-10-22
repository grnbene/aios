#!/usr/bin/env python3
"""`grain serve` against a ui.pane program: the page, events, sessions, escaping and the cross-site guard.
No browser needed.

    web_ui_test.py <grain> <counter.gr>      starts `grain serve` itself
    GRAIN_WEB_PORT=n web_ui_test.py          tests the server already listening there (aios-webserver's grain_ui)
"""
import gzip, http.client, json, re, socket, subprocess, sys, time, urllib.parse

import os
if os.environ.get("GRAIN_WEB_PORT"):
    port, server = int(os.environ["GRAIN_WEB_PORT"]), None
else:
    grain, program = sys.argv[1], sys.argv[2]
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    server = subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
failed = False

def request(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    if r.getheader("Content-Encoding") == "gzip": data = gzip.decompress(data)
    return r.status, {k.lower(): v for k, v in r.getheaders()}, data.decode()

def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True

class Browser:
    def __init__(self):
        status, headers, html = request("GET", "/")
        self.cookie = headers["set-cookie"].split(";")[0]
        self.html = html
    def event(self, action, **fields):
        body = urllib.parse.urlencode([("action", action)] + [("f." + k, v) for k, v in fields.items()])
        status, _, text = request("POST", "/_ui/event", body, {"X-Grain-UI": "1", "Cookie": self.cookie, "Content-Type": "application/x-www-form-urlencoded"})
        d = json.loads(text)
        self.html = d["html"]
        return d
    def text(self): return re.sub(r"<[^>]+>", "|", self.html)
    def action(self, label): return re.search(r'data-act="([^"]+)">' + re.escape(label) + "<", self.html).group(1)
    def enter(self, field): return re.search(r'data-id="' + field + r'"[^>]*data-enter="([^"]+)"', self.html).group(1)

try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)

    status, headers, html = request("GET", "/")
    check("the page is served", 200, status)
    check("it is HTML", "text/html; charset=utf-8", headers["content-type"])
    check("the title is the pane's", True, "<title>Counter</title>" in html)
    check("the widgets are in it", True, "Clicked 0 times" in html and 'class="w-btn primary"' in html and 'data-id="note"' in html)
    check("the cookie cannot be read by script or sent cross-site", True, "HttpOnly" in headers["set-cookie"] and "SameSite=Strict" in headers["set-cookie"])
    check("a content security policy", True, "default-src 'none'" in headers["content-security-policy"])
    if os.environ.get("GRAIN_WEB_PORT"):   # aios-webserver compresses; `grain serve` does not
        gz = request("GET", "/", None, {"Accept-Encoding": "gzip"})
        check("the page is gzipped when asked, and intact", True, gz[1].get("content-encoding") == "gzip" and "Clicked 0 times" in gz[2])

    a = Browser()
    a.event(a.action("Add one"))
    d = a.event(a.action("Add one"))
    check("a click calls the grain function, twice", True, "Clicked 2 times" in d["html"])
    a.event(a.enter("note"), note="hello world")
    check("Enter calls on_enter with the field text", True, "hello world" in a.text())
    check("the app cleared the field (ui.set_field)", True, 'data-id="note" value=""' in a.html)
    a.event(a.action("Save"), note="<b>bold</b> & \"quoted\"")
    check("text is escaped", True, "&lt;b&gt;bold&lt;/b&gt; &amp; &quot;quoted&quot;" in a.html and "<b>bold</b>" not in a.html)
    d = a.event("", note="a draft")
    check("fields travel with every event", True, 'value="a draft"' in d["html"])

    _, _, text = request("POST", "/_ui/event", urllib.parse.urlencode([("action", "a-from-an-old-page")]), {"X-Grain-UI": "1", "Cookie": a.cookie})
    check("an action that no longer exists does nothing", True, "Clicked 2 times" in json.loads(text)["html"])
    a.event(a.action("Reset"))
    check("Reset", True, "Clicked 0 times" in a.html)

    b = Browser()
    check("another browser has its own session", True, "Clicked 0 times" in b.html and "no notes yet" in b.text())
    check("and a session never leaks into another", False, "hello world" in b.text())

    check("an event without X-Grain-UI is refused", 403, request("POST", "/_ui/event", "action=x", {"Cookie": a.cookie})[0])
    check("GET on the event endpoint is refused", 405, request("GET", "/_ui/event", None, {"Cookie": a.cookie})[0])
    check("POST to the page is refused", 405, request("POST", "/", "x=1")[0])
    check("a bogus cookie starts a fresh session", 200, request("GET", "/", None, {"Cookie": "grain_sid=deadbeef"})[0])
    check("favicon is not a session", 204, request("GET", "/favicon.ico")[0])
    check("HEAD has no body", "", request("HEAD", "/")[2])
finally:
    if server: server.kill(); server.wait()
sys.exit(1 if failed else 0)
