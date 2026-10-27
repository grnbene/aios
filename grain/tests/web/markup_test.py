#!/usr/bin/env python3
"""`grain serve` against a ui.pane whose view is el() markup: actions on elements, a bound field, server-side
sort that survives a re-render, a widget inside markup, escaping, and the native client's tree.

    markup_test.py <grain> <markup.gr>
"""
import http.client, json, re, socket, subprocess, sys, time, urllib.parse

grain, program = sys.argv[1], sys.argv[2]
with socket.socket() as s:
    s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
server = subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
failed = False

def request(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read().decode()

def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True

def names(html): return re.findall(r'<td class="who">([^<]*)</td>', html)

try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    status, headers, html = request("GET", "/")
    check("the page is served", 200, status)
    check("the markup is in it, inside the web UI's page", True, '<div id="app"' in html and '<table><thead><tr><th>Name</th>' in html)
    check("the pane's css is in the page", True, ".sortable { cursor: pointer }" in html)
    check("text is escaped", True, "Ann &lt;a&amp;b&gt;" in html and "<a&b>" not in html)
    check("busiest first", ["Bob", "Cy", "Ann &lt;a&amp;b&gt;"], names(html))
    check("a widget inside markup is a widget", True, re.search(r'<div class="w-text bold"[^>]*>clicks 0</div>', html) is not None)
    act = re.search(r'<th class="sortable" data-dir="desc" data-act="([^"]+)">Touches</th>', html)
    check("on_click is an action on the element", True, act is not None)
    check("no function leaks into the markup", False, "on_click" in html)
    cookie = headers["set-cookie"].split(";")[0]

    def event(action, **fields):
        body = urllib.parse.urlencode([("action", action)] + [("f." + k, v) for k, v in fields.items()])
        _, _, text = request("POST", "/_ui/event", body, {"X-Grain-UI": "1", "Cookie": cookie, "Content-Type": "application/x-www-form-urlencoded"})
        return json.loads(text)["html"]

    h = event(act.group(1))
    check("the click ran: the sort turned", ["Ann &lt;a&amp;b&gt;", "Cy", "Bob"], names(h))
    check("the header says so", True, 'data-dir="asc"' in h)
    check("and the widget saw it", True, ">clicks 1</div>" in h)
    h = event("")
    check("a render with nothing new keeps the sort (it is the session's)", ["Ann &lt;a&amp;b&gt;", "Cy", "Bob"], names(h))
    bound = re.search(r'<input type="date" data-id="start" value="2026-10-01">', h)
    check("bind makes the input a field", True, bound is not None)
    run = re.search(r'data-act="([^"]+)">Run</button>', h).group(1)
    h = event(run, start="2026-09-15")
    check("the action got the field's value", True, '<p id="start">from 2026-09-15</p>' in h)
    check("the input shows the new value", True, 'data-id="start" value="2026-09-15"' in h)

    _, th, tree = request("GET", "/", None, {"Accept": "application/x-grain-ui+json", "Cookie": cookie})
    t = json.loads(tree)["tree"]
    check("a native client gets the markup as markup", ("html", True), (t["widget"], t["html"].startswith('<div class="page">')))
finally:
    server.terminate()
    try: server.wait(timeout=5)
    except subprocess.TimeoutExpired: server.kill()
sys.exit(1 if failed else 0)
