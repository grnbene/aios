#!/usr/bin/env python3
"""The widget set in all three forms: HTML for a browser, the widgets as data for a native client, and a
native client drawing them.   widgets_test.py <grain> <widgets.gr>"""
import http.client, json, os, socket, subprocess, sys, time
grain, program = sys.argv[1], sys.argv[2]
failed = False
def check(name, ok):
    global failed
    print(("ok   " if ok else "FAIL ") + name); failed |= not ok
with socket.socket() as s:
    s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
server = subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    c.request("GET", "/"); html = c.getresponse().read().decode()
    check("heading", 'class="w-heading"' in html)
    check("badges, with accents and a dot", 'class="w-badge ok dot"' in html and 'class="w-badge bad"' in html)
    check("progress shows a percentage", 'class="w-bar"' in html and "Upload" in html)
    check("a gauge shows its value with a suffix", "42 C" in html)
    check("sparklines and a chart are SVG", html.count("<svg") >= 3 and "<polygon" in html)
    check("bars are gauges against the largest, with a prefix", "$340" in html and "$90" in html)
    check("a badge column in a table", '<span class="w-badge warn">open</span>' in html and '<span class="w-badge ok">shipped</span>' in html)
    c.request("GET", "/", headers={"Accept": "application/x-grain-ui"}); tree = json.loads(c.getresponse().read())
    kinds = set()
    def walk(n):
        kinds.add(n["widget"])
        for k in n.get("children", []): walk(k)
    walk(tree["tree"])
    check("the same widgets as data", {"heading", "badge", "tile", "progress", "gauge", "sparkline", "chart", "table"} <= kinds)
    r = subprocess.run([grain, "open", "http://127.0.0.1:%d/" % port, "--frames", "10", "--dump"], env=dict(os.environ, GRAIN_VIDEO="dummy"), capture_output=True, text=True, timeout=60)
    check("a native client draws them", r.returncode == 0 and "Widgets" in r.stdout and "Ticks=" in r.stdout)
finally:
    server.kill(); server.wait()
sys.exit(1 if failed else 0)
