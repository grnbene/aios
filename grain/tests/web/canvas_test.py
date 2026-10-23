#!/usr/bin/env python3
"""A pane drawn by hand (create_pane with on_draw) in a browser: its frames as PNG, pushed as they change; the
page's clicks become the pane's own events.   canvas_test.py <grain> <bounce.gr>"""
import http.client, json, socket, subprocess, sys, threading, time, zlib, struct, base64
grain, program = sys.argv[1], sys.argv[2]
failed = False
def check(name, ok):
    global failed
    print(("ok   " if ok else "FAIL ") + name); failed |= not ok
with socket.socket() as s:
    s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
server = subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def png_ok(b64):
    raw = base64.b64decode(b64)
    if raw[:8] != b"\x89PNG\r\n\x1a\n": return False
    w, h = struct.unpack(">II", raw[16:24])
    at, idat = 8, b""
    while at < len(raw):
        n = struct.unpack(">I", raw[at:at + 4])[0]; t = raw[at + 4:at + 8]; d = raw[at + 8:at + 8 + n]
        if struct.unpack(">I", raw[at + 8 + n:at + 12 + n])[0] != zlib.crc32(t + d) & 0xffffffff: return False
        if t == b"IDAT": idat += d
        at += 12 + n
    return len(zlib.decompress(idat)) == h * (w * 3 + 1)   # every row: a filter byte and RGB
try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    c.request("GET", "/"); r = c.getresponse(); page = r.read().decode(); ck = r.getheader("Set-Cookie").split(";")[0]
    check("the page shows the first frame", r.status == 200 and 'id="cv"' in page and "data:image/png;base64," in page)
    frames = []
    def watch():
        w = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        w.request("GET", "/", headers={"Accept": "text/event-stream", "Cookie": ck})
        resp = w.getresponse(); t0 = time.time()
        while time.time() - t0 < 2:
            line = resp.fp.readline().decode()
            if line.startswith("data: "): frames.append(json.loads(line[6:]))
    t = threading.Thread(target=watch); t.start(); t.join()
    check("moving pictures are pushed with nothing asked", len(frames) >= 8 and len(set(f["png"] for f in frames)) >= 8)
    check("each frame is a well-formed PNG", all(png_ok(f["png"]) for f in frames[:5]))
    hdr = {"X-Grain-UI": "1", "Cookie": ck, "Content-Type": "application/x-www-form-urlencoded"}
    c.request("POST", "/", "type=mouse_down&x=200&y=150&button=left", hdr); json.loads(c.getresponse().read())
    c.request("POST", "/", "type=mouse_up&x=200&y=150&button=left", hdr); d = json.loads(c.getresponse().read())
    check("a click is answered with a new frame", d.get("canvas") and png_ok(d["png"]))
    c.request("POST", "/", "type=launch_missiles", hdr); check("an event type a window never sends is ignored", c.getresponse().status == 200)
    c.request("GET", "/", headers={"Accept": "application/x-grain-ui", "Cookie": ck}); t = json.loads(c.getresponse().read())
    check("a native client is told it is a drawn pane, with the picture", t.get("canvas") is True and png_ok(t.get("png", "")))
finally:
    server.kill(); server.wait()
sys.exit(1 if failed else 0)
