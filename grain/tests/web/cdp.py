"""A page driven over the Chrome DevTools protocol, with no library: a websocket by hand."""
import socket, json, base64, os, struct, time, urllib.request, sys
DEBUG_PORT = 9333   # set by the caller before the first Page

def ws_connect(url):
    host, port, path = "127.0.0.1", DEBUG_PORT, url.split(":%d" % DEBUG_PORT, 1)[1]
    s = socket.create_connection((host, port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(4096)
    return s
def send(s, obj):
    data = json.dumps(obj).encode(); mask = os.urandom(4)
    hdr = bytes([0x81]); n = len(data)
    if n < 126: hdr += bytes([0x80 | n])
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
def recv(s):
    def rd(n):
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c: raise EOFError
            b += c
        return b
    h = rd(2); n = h[1] & 0x7F
    if n == 126: n = struct.unpack(">H", rd(2))[0]
    elif n == 127: n = struct.unpack(">Q", rd(8))[0]
    return json.loads(rd(n))
def new_tab():
    return json.load(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/json/new" % DEBUG_PORT, method="PUT")))

class Page:
    def __init__(self, url, tab=None):
        if tab is None:
            tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % DEBUG_PORT))
            tab = [t for t in tabs if t["type"] == "page"][0]
        self.s = ws_connect(tab["webSocketDebuggerUrl"]); self.id = 0
        self.call("Page.enable"); self.call("Page.navigate", url=url); time.sleep(1.0)
    def call(self, method, **params):
        self.id += 1; send(self.s, {"id": self.id, "method": method, "params": params})
        while True:
            m = recv(self.s)
            if m.get("id") == self.id: return m
    def js(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True)
        return r["result"]["result"].get("value")
