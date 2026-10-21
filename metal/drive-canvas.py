#!/usr/bin/env python3
"""Drive the machine as an operator would leave it running.

Not a demonstration of commands: a demonstration that this machine does work
nobody is typing. It boots the real ISO, brings its own TCP/IP up, is given ONE
standing directive, and is then left alone at the prompt. What has to be true
afterwards is that the directive ran several times on its own, that the machine
fetched a page over its own network stack and kept what the page said, that it
noticed when the page CHANGED, and that its canvas — no windows, no files —
renders the lifecycle that produced all of it.

  metal/drive-canvas.py [iso] [work-dir]
"""
import http.server, json, os, re, socket, socketserver, struct, subprocess, sys, threading, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

here = os.path.dirname(os.path.abspath(__file__))
iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "metal", "aios-metal.iso")
work = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "build", "metal", "canvas")
os.makedirs(work, exist_ok=True)

fw = firmware_args(force_uefi=True)
if not fw:
    print("SKIP: no OVMF firmware on this host")
    sys.exit(0)

# ---- the outside world, served from this host so the drive is repeatable ----
PAGE = {"body": b"""<html><head><title>Horizon</title></head><body>
<h1>Horizon</h1><p>The first thing this machine ever read on its own.</p>
<script>ignored()</script><p>Edition one.</p></body></html>"""}

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = PAGE["body"]
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

httpd = socketserver.TCPServer(("0.0.0.0", 0), Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()

ser_path = os.path.join(work, "serial.sock")
qmp_path = os.path.join(work, "qmp.sock")
for p in (ser_path, qmp_path):
    if os.path.exists(p):
        os.remove(p)

cmdline = ["qemu-system-x86_64", *fw, "-m", "1024", "-smp", "2", "-display", "none", "-vga", "std",
           "-no-reboot", "-cdrom", iso, "-boot", "d",
           "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56",
           "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_path, "-serial", "chardev:ser0",
           "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
if os.path.exists("/dev/kvm"):
    cmdline += ["-enable-kvm", "-cpu", "host"]
q = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ok = True
def check(what, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print(("  ok    " if cond else "  FAIL  ") + what + (("  |  " + detail) if detail else ""), flush=True)

def connect(path, deadline=60):
    end = time.time() + deadline
    while time.time() < end:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.25)
    raise SystemExit("FAIL: %s never appeared" % path)

ser = connect(ser_path)
ser.settimeout(1.0)
buf = b""

def expect(pattern, timeout=180):
    global buf
    rx = re.compile(pattern.encode())
    end = time.time() + timeout
    while time.time() < end:
        m = rx.search(buf)
        if m:
            out, buf = buf[:m.end()], buf[m.end():]
            return re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", out.decode(errors="replace"))
        try:
            d = ser.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d
    raise SystemExit("TIMEOUT waiting for %r; tail:\n%s" % (pattern, buf[-2500:].decode(errors="replace")))

def cmd(line, timeout=180):
    ser.sendall((line + "\r").encode())
    return expect(r"\naios> ", timeout)

class Qmp:
    def __init__(self, path):
        self.s = connect(path)
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")
    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            m = json.loads(line)
            if "error" in m:
                raise RuntimeError("%s: %s" % (name, m["error"]))
            if "return" in m:
                return m["return"]

class Shot:
    def __init__(self, qmp, tag):
        ppm = os.path.join(work, "canvas-%s.ppm" % tag)
        if os.path.exists(ppm):
            os.remove(ppm)
        qmp.cmd("screendump", filename=ppm)
        last = -1
        for _ in range(80):
            time.sleep(0.25)
            if not os.path.exists(ppm):
                continue
            size = os.path.getsize(ppm)
            if size > 1000 and size == last:
                break
            last = size
        data = open(ppm, "rb").read()
        parts = data.split(maxsplit=4)
        self.w, self.h = int(parts[1]), int(parts[2])
        self.pix = parts[4]
        if len(self.pix) < self.w * self.h * 3:
            raise SystemExit("FAIL: screendump %s truncated" % tag)
    def px(self, x, y):
        i = (y * self.w + x) * 3
        return self.pix[i], self.pix[i + 1], self.pix[i + 2]
    def colours(self, step=3):
        return {self.px(x, y) for y in range(0, self.h, step) for x in range(0, self.w, step)}
    def ink_in(self, x0, y0, x1, y1, step=2):
        """Pixels in that region that are not the canvas background."""
        n = 0
        for y in range(y0, min(y1, self.h), step):
            for x in range(x0, min(x1, self.w), step):
                p = self.px(x, y)
                if p[0] > 40 or p[1] > 40 or p[2] > 40:
                    n += 1
        return n
    def png(self, path):
        raw = b"".join(b"\x00" + self.pix[y * self.w * 3:(y + 1) * self.w * 3] for y in range(self.h))
        def chunk(t, d):
            c = t + d
            return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
        open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
                               + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
                               + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
        print("      wrote %s" % path, flush=True)

print("=== the machine comes up")
banner = expect(r"aios> ", 240)
for line in banner.splitlines():
    if line.startswith("[metal]"):
        print("  | " + line)

print("\n=== it brings its own TCP/IP up — no host stack under it")
o = cmd("host net up", 120)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the interface came up with an address", "up: ip " in o)

print("\n=== it looks at the world, over its own network stack")
url = "http://10.0.2.2:%d/" % port
o = cmd("sense %s" % url, 120)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("it read the page and knows its title", "Horizon" in o)
check("the script on the page is not what the page says", "ignored()" not in o)
check("and it kept what it read as an object", "now knows" in o or "already knew" in o)

print("\n=== it is given ONE standing order, and then left alone")
cmd("entity create horizon => h")
cmd("cap grant $h network * consume")
cmd("cap grant $h storage * write")
o = cmd("directive add scan 3s as=$h sense %s" % url)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the order is standing", "it runs whenever this machine is idle" in o)

print("      (typing nothing for 14 seconds)")
time.sleep(14)
o = cmd("directive list")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
runs = re.search(r"scan\s+\S+\s+(\d+)", o)
check("it carried the order out while nobody was typing", runs and int(runs.group(1)) >= 1,
      "%s runs with no input" % (runs.group(1) if runs else "?"))

print("\n=== the world changes, and it notices")
PAGE["body"] = b"""<html><head><title>Horizon</title></head><body>
<h1>Horizon</h1><p>Edition two: something happened while nobody was looking.</p></body></html>"""
time.sleep(9)
o = cmd("store list")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip())[-600:])
check("the second edition became a second thing it knows", o.count("url=") >= 2,
      "%d objects on the append log" % o.count("url="))
check("and it did not keep a copy per fetch", o.count("url=") <= 3)

print("\n=== the canvas: no windows, no files")
qmp = Qmp(qmp_path)
ser.sendall(b"host canvas\r")
time.sleep(10)
shot = Shot(qmp, "01")
shot.png(os.path.join(work, "canvas-01-lifecycle.png"))
check("the screen is graphical, not a text console", len(shot.colours()) > 8,
      "%d distinct colours at %dx%d" % (len(shot.colours()), shot.w, shot.h))
third = shot.w // 3
check("the identity column drew", shot.ink_in(20, 120, third - 20, shot.h - 140) > 200)
check("the authority and directive column drew", shot.ink_in(third + 20, 120, third * 2 - 20, shot.h - 140) > 200)
check("the column of what it knows drew", shot.ink_in(third * 2 + 20, 120, shot.w - 20, shot.h - 140) > 200)
check("the lanes along the bottom drew", shot.ink_in(20, shot.h - 96, shot.w - 20, shot.h - 10) > 100)

print("\n=== and it gives the screen back")
qmp.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "esc"}}},
                                    {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "esc"}}}])
ser.sendall(b"\r")
# The console prints its prompt straight after the canvas' parting line, with no
# newline between them — so this waits for the prompt itself, not a fresh line.
o = expect(r"aios> ", 60)
after = Shot(qmp, "02")
after.png(os.path.join(work, "canvas-02-back.png"))
check("the canvas closed and the console has the screen", len(after.colours()) < len(shot.colours()))

ser.sendall(b"quit\r")
time.sleep(1)
q.kill()
q.wait()
httpd.shutdown()

print("\nCANVAS: " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
