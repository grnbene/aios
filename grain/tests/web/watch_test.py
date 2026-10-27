#!/usr/bin/env python3
"""`grain serve` against a ui.pane with "watch": a change the watch sees reaches an open page's stream with no
action; a watch that sees nothing new sends nothing.

    watch_test.py <grain> <watch.gr>
"""
import http.client, json, os, re, socket, subprocess, sys, tempfile, threading, time

grain, program = sys.argv[1], sys.argv[2]
d = tempfile.mkdtemp()
data = os.path.join(d, "value.txt")
open(data, "w").write("1\n")
with socket.socket() as s:
    s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
server = subprocess.Popen([grain, "serve", program, "--port", str(port)], env=dict(os.environ, WATCH_FILE=data),
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
failed = False
def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True
try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15); c.request("GET", "/"); r = c.getresponse()
    cookie = r.getheader("Set-Cookie").split(";")[0]; html = r.read().decode()
    check("the first view", True, '<p id="v">value 1</p>' in html)
    frames = []
    def listen():
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
        c.request("GET", "/", headers={"Accept": "text/event-stream", "Cookie": cookie}); r = c.getresponse()
        buf, t0 = b"", time.time()
        while time.time() - t0 < 6:
            ch = r.fp.read1(65536)
            if not ch: break
            buf += ch
            while b"\n\n" in buf:
                ev, buf = buf.split(b"\n\n", 1)
                for line in ev.decode().splitlines():
                    if line.startswith("data: "): frames.append((time.time(), json.loads(line[6:]).get("html", "")))
    t = threading.Thread(target=listen, daemon=True); t.start()
    time.sleep(1.5)
    quiet = len(frames)
    check("nothing new: only the stream's first frame", 1, quiet)
    t_change = time.time()
    open(data, "w").write("2\n")
    t.join(7)
    after = [f for f in frames if f[0] > t_change and "value 2" in f[1]]
    check("the change reached the open page with no action", True, bool(after))
    if after: print("     %.0f ms after the file changed" % ((after[0][0] - t_change) * 1000))
    check("one frame for one change", 1, len([f for f in frames if f[0] > t_change]))
finally:
    server.terminate()
    try: server.wait(timeout=5)
    except subprocess.TimeoutExpired: server.kill()
    try: os.remove(data); os.rmdir(d)
    except OSError: pass
sys.exit(1 if failed else 0)
