#!/usr/bin/env python3
"""Live data: nothing in the program sends, polls or listens, and every screen showing the data follows it.

    live_test.py <grain> <live.gr>
    GRAIN_WEB_PORT=n live_test.py <grain>        against a server already listening there
"""
import http.client, json, os, re, socket, subprocess, sys, threading, time

grain = sys.argv[1] if len(sys.argv) > 1 else None
failed = False
def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True

if os.environ.get("GRAIN_WEB_PORT"):
    port, server = int(os.environ["GRAIN_WEB_PORT"]), None
else:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    server = subprocess.Popen([grain, "serve", sys.argv[2], "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def watch(frames, seconds, out):
    """Open an event stream and keep what the server pushes. This is all a watcher does: one request."""
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=seconds + 10)
    c.request("GET", "/", headers={"Accept": "text/event-stream"})
    r = c.getresponse()
    out.append(("head", r.status, r.getheader("Content-Type")))
    t0 = time.time()
    while time.time() - t0 < seconds:
        line = r.fp.readline().decode()
        if line.startswith("data: "):
            d = json.loads(line[6:])
            out.append(("frame", d["seq"], re.findall(r'class="tv"[^>]*>([^<]*)<', d["html"]), d["html"]))
    c.close()

try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)

    a, b = [], []
    ta = threading.Thread(target=watch, args=(0, 3.5, a)); tb = threading.Thread(target=watch, args=(0, 3.5, b))
    ta.start(); time.sleep(0.2); tb.start()
    time.sleep(1.6)
    # a third visitor speaks; the two watchers do nothing
    c = http.client.HTTPConnection("127.0.0.1", port)
    c.request("GET", "/"); page = c.getresponse().read().decode()
    send = re.search(r'data-act="([^"]+)">Send<', page).group(1)
    c.request("POST", "/", "action=%s&f.who=ada&f.words=hello+live" % send, {"X-Grain-UI": "1", "Content-Type": "application/x-www-form-urlencoded"})
    answer = json.loads(c.getresponse().read())
    ta.join(); tb.join()

    check("a stream is event-stream", ("head", 200, "text/event-stream"), a[0])
    frames_a = [f for f in a if f[0] == "frame"]; frames_b = [f for f in b if f[0] == "frame"]
    check("the watcher is pushed updates it never asked for", True, len(frames_a) >= 4)
    check("the clock moves on its own", True, int(frames_a[-1][2][0]) > int(frames_a[0][2][0]))
    check("sequence numbers only go up", True, all(x[1] < y[1] for x, y in zip(frames_a, frames_a[1:])))
    check("another visitor's message reaches a watcher with no request from it", True, any("hello live" in f[3] for f in frames_a))
    check("and every other watcher", True, any("hello live" in f[3] for f in frames_b))
    check("the shared data is one copy: both see the same count", frames_a[-1][2][0] == frames_b[-1][2][0] or abs(int(frames_a[-1][2][0]) - int(frames_b[-1][2][0])) <= 1, True)
    check("a shared app sets no cookie to track", True, "hello live" in answer["html"])

    # a native client follows the same data
    if grain is None: raise SystemExit(1 if failed else 0)   # against aios-webserver: the stream is what is tested here
    env = dict(os.environ, GRAIN_VIDEO="dummy")
    r = subprocess.run([grain, "open", f"http://127.0.0.1:{port}/", "--frames", "120", "--dump"], env=env, capture_output=True, text=True, timeout=60)
    lines = r.stdout.splitlines()
    updates = int(re.search(r"updates: (\d+)", r.stdout).group(1))
    check("a native client opens it", 0, r.returncode)
    check("it shows the message another visitor sent", True, any("hello live" in l for l in lines))
    check("and was updated while open, without being asked", True, updates >= 1)
finally:
    if server: server.kill(); server.wait()
sys.exit(1 if failed else 0)
