#!/usr/bin/env python3
"""Runs grain against a fake X server that has two keyboard layouts (US and
Russian, with AltGr levels) and sends it key events whose state names the
active layout, then checks what the pane received. No display is needed.

    x11_layouts.py <grain> <layouts.gr>
"""
import os, socket, struct, subprocess, sys, tempfile, threading

grain, program = sys.argv[1], sys.argv[2]
NUM = 96
path = "/tmp/.X11-unix/X%d" % NUM
abstract = "\0" + path

# keysyms: columns are group1 (plain, shift), group2 (plain, shift), then the AltGr
# levels of group1 and of group2. 0 means the layout has no symbol there.
CYR = dict(ef=0x06c6, EF=0x06e6, ve=0x06d7, VE=0x06f7)
KEYMAP = {
    38: [ord('a'), ord('A'), CYR["ef"], CYR["EF"], 0xe1, 0xc1, 0, 0],      # a  | ф  | á
    40: [ord('d'), ord('D'), CYR["ve"], CYR["VE"], 0xdf, 0, 0, 0],         # d  | в  | ß
    10: [ord('1'), ord('!'), ord('1'), ord('!'), 0, 0, 0, 0],              # 1 !
    53: [ord('x'), ord('X'), 0x1000436, 0x1000416, 0, 0, 0, 0],            # x  | ж Ж (Unicode keysyms)
    36: [0xff0d] * 8,                                                      # Return
}
KPK, MIN_KC, MAX_KC = 8, 8, 255

def pad4(n): return (4 - n % 4) % 4

class Server(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(abstract)
        self.sock.listen(1)
        self.atoms, self.wid, self.error = {}, 0, None
    def run(self):
        try:
            c, _ = self.sock.accept()
            self.serve(c)
        except Exception as e:  # reported by the main thread
            self.error = repr(e)
    def read(self, c, n):
        b = b""
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d: raise EOFError
            b += d
        return b
    def serve(self, c):
        head = self.read(c, 12)
        nlen, dlen = struct.unpack("<HH", head[6:10])
        self.read(c, nlen + pad4(nlen) + dlen + pad4(dlen))
        vendor = b"fake"
        data = struct.pack("<IIIIHHBBBBBBBB4x", 1, 0x04000000, 0x001fffff, 256, len(vendor), 65535,
                           1, 1, 0, 0, 32, 32, MIN_KC, MAX_KC)
        data += vendor + b"\0" * pad4(len(vendor))
        data += struct.pack("<BBB5x", 24, 32, 32)                                   # one pixmap format
        data += struct.pack("<IIIIIHHHHHHIBBBB", 0x100, 0x20, 0xffffff, 0, 0, 800, 600, 200, 150, 1, 1, 0x21, 0, 0, 24, 0)
        c.sendall(struct.pack("<BxHHH", 1, 11, 0, len(data) // 4) + data)
        seq = 0
        while True:
            h = self.read(c, 4)
            op, length = h[0], struct.unpack("<H", h[2:4])[0]
            body = self.read(c, length * 4 - 4)
            seq += 1
            if op == 16:                                                             # InternAtom
                n = struct.unpack("<H", body[0:2])[0]
                name = body[4:4 + n].decode()
                atom = self.atoms.setdefault(name, 100 + len(self.atoms))
                c.sendall(struct.pack("<BxHII", 1, seq, 0, atom) + b"\0" * 20)
            elif op == 101:                                                          # GetKeyboardMapping
                count = body[1]
                syms = b""
                for kc in range(MIN_KC, MIN_KC + count):
                    row = KEYMAP.get(kc, [0] * KPK)
                    syms += struct.pack("<%dI" % KPK, *row)
                c.sendall(struct.pack("<BBHI", 1, KPK, seq, count * KPK) + b"\0" * 24 + syms)
            elif op == 1:                                                            # CreateWindow
                self.wid = struct.unpack("<I", body[0:4])[0]
            elif op == 8:                                                            # MapWindow: type
                self.type_keys(c)
                return
    def key(self, c, code, state):
        for press in (2, 3):
            c.sendall(struct.pack("<BBHIIIIhhhhHBx", press, code, 0, 0, 0, self.wid, 0, 0, 0, 10, 10, state, 1))
    def type_keys(self, c):
        G, SHIFT, CAPS, ALTGR = 0x2000, 1, 2, 0x80
        for code, state in [(38, 0), (38, G), (38, G | SHIFT), (38, G | CAPS), (10, G | SHIFT), (10, CAPS),
                            (40, ALTGR), (40, G | ALTGR), (38, ALTGR), (53, G), (53, G | SHIFT), (53, 0), (36, 0)]:
            self.key(c, code, state)
        # WM_DELETE_WINDOW: closes the pane, which ends run_panes()
        pa, da = self.atoms.get("WM_PROTOCOLS"), self.atoms.get("WM_DELETE_WINDOW")
        c.sendall(struct.pack("<BBHII5I", 33, 32, 0, self.wid, pa, da, 0, 0, 0, 0))
        self.read_until_closed(c)
    def read_until_closed(self, c):
        c.settimeout(10)
        try:
            while c.recv(4096): pass
        except Exception:
            pass

srv = Server()
srv.start()
env = dict(os.environ, DISPLAY=":%d" % NUM)
env.pop("GRAIN_VIDEO", None); env.pop("SDL_VIDEODRIVER", None)
out = subprocess.run([grain, program], env=env, capture_output=True, text=True, timeout=30)
lines = dict(l.split(": ", 1) for l in out.stdout.splitlines() if ": " in l)
# US a; Russian ф, Ф (shift), Ф (caps lock); Russian shift+1 is "!"; caps lock leaves 1 alone;
# AltGr: ß, then в (Russian has no AltGr level, so the plain letter), then á; ж, Ж, then US x
want_text = "aфФФ!1ßвáжЖx"
want_keys = "a ф ф ф 1 1 d в a ж ж x return"
ok = lines.get("text") == want_text and lines.get("keys") == want_keys and out.returncode == 0 and not srv.error
if not ok:
    print("grain exit %d, server error %s" % (out.returncode, srv.error))
    print("stdout:", out.stdout)
    print("stderr:", out.stderr)
    print("wanted text %r keys %r" % (want_text, want_keys))
    sys.exit(1)
print("layouts ok")
