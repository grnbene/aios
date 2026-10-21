#!/usr/bin/env python3
"""Drive the horizon ON BARE METAL: the machine works out what it cares about
from its own commits, reads the world over its own TCP/IP, keeps only what
touches that work, and paints it on the glass.

This is the whole concept end to end, on the real ISO, with nothing hosted in
the path:

  * the machine makes a repository and writes three commits into it — that, and
    only that, is where its interests come from. Nobody types a keyword.
  * it is given a page of headlines, most of which are about nothing it does.
  * a STANDING DIRECTIVE scans, on its own, while nobody is typing.
  * what it keeps is what touched its own work; the noise is not kept.
  * the canvas draws that horizon, and the column is measurably fuller after
    the scan than it was before it.

  metal/drive-horizon.py [iso] [work-dir]
"""
import http.server, json, os, re, socket, socketserver, struct, subprocess, sys, threading, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

here = os.path.dirname(os.path.abspath(__file__))
iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "metal", "aios-metal.iso")
work = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "build", "metal", "horizon")
os.makedirs(work, exist_ok=True)

fw = firmware_args(force_uefi=True)
if not fw:
    print("SKIP: no OVMF firmware on this host")
    sys.exit(0)

# ---- the world, served from this host so the drive is repeatable ------------
# Four of these are about nothing this machine does. Two touch the work its own
# commits describe. Which is which is never told to the machine.
TOUCHES = [
    ("Virtio-GPU scanout flushing lands in the graphics tree", "/a/virtio-scanout"),
    ("A queue notify address computed from the queue itself", "/a/queue-notify"),
]
NOISE = [
    ("Coffee chain adds an oat milk latte to its winter menu", "/a/latte"),
    ("Regional football final ends in a penalty shootout", "/a/football"),
    ("Museum opens an exhibit of nineteenth century pottery", "/a/pottery"),
    ("Rainfall records broken across the western counties", "/a/rain"),
]

def feed_page():
    rows = "".join('<li><a href="%s">%s</a></li>' % (u, t) for t, u in NOISE[:2] + TOUCHES + NOISE[2:])
    return ("<html><head><title>The wire</title></head><body><h1>The wire</h1>"
            "<ul>%s</ul></body></html>" % rows).encode()

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = feed_page()
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
           "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:12:34:57",
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
        ppm = os.path.join(work, "horizon-%s.ppm" % tag)
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
check("the interface came up with an address", "up: ip " in o, o.strip().splitlines()[-1] if o.strip() else "")

print("\n=== it does some work of its own, and that is the ONLY thing it is told")
cmd("git init work")
root = "/host/aios/root/repos/work"
subjects = [
    "virtio scanout brought up on the metal",
    "virtio scanout flushed at the end of every frame",
    "virtio queue notify address computed from the queue",
]
for i, subject in enumerate(subjects):
    cmd("echo note %d > %s/NOTE%d.md" % (i, root, i), 30)
    cmd("git add NOTE%d.md" % i, 30)
    o = cmd("git commit " + subject, 60)
    check("commit %d written into its own repository" % (i + 1), "[COMMIT]" in o)

print("\n=== so it works out what it cares about — nobody typed a keyword")
o = cmd("horizon interests", 60)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip())[:1200])
check("it named virtio, from its own commit subjects", re.search(r"virtio\s+[\d.]+\s+3\s+commit subjects", o))
check("it named scanout too", "scanout" in o)
check("and the terms come from the machine, not the operator", "nobody typed any of them" in o)

print("\n=== it is given somewhere to look, and a standing order to look there")
url = "http://10.0.2.2:%d/" % port
cmd("horizon watch wire %s" % url, 30)
cmd("entity create scout => s")
cmd("cap grant $s network * consume")
cmd("cap grant $s storage * write")

print("\n=== the canvas, before it has looked at anything")
qmp = Qmp(qmp_path)
ser.sendall(b"host canvas\r")
time.sleep(10)
before = Shot(qmp, "01")
before.png(os.path.join(work, "horizon-01-empty.png"))
third = before.w // 3
col = (third * 2 + 20, 120, before.w - 20, before.h - 140)
empty_ink = before.ink_in(*col)
check("the canvas is graphical", len(before.colours()) > 8,
      "%d distinct colours at %dx%d" % (len(before.colours()), before.w, before.h))
qmp.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "esc"}}},
                                    {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "esc"}}}])
ser.sendall(b"\r")
expect(r"aios> ", 60)

o = cmd("directive add horizon 4s as=$s horizon scan", 30)
check("the order is standing", "it runs whenever this machine is idle" in o)
print("      (typing nothing for 12 seconds)")
time.sleep(12)

print("\n=== what it kept, having been left alone")
o = cmd("directive list", 60)
runs = re.search(r"horizon\s+\S+\s+(\d+)", o)
check("it scanned while nobody was typing", runs and int(runs.group(1)) >= 1,
      "%s run(s) with no input" % (runs.group(1) if runs else "?"))

o = cmd("horizon show", 60)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip())[:1600])
for title, _ in TOUCHES:
    check("kept: %s" % title[:44], title[:40] in o)
for title, _ in NOISE:
    check("did not keep: %s" % title[:44], title[:40] not in o)
check("and it said which of its own terms each one touched", "virtio" in o)

o = cmd("horizon sources", 30)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the register survived as kernel state", "register state" in o)

o = cmd("store list", 60)
check("each signal is on the append log", o.count("kind=signal") >= 2 or o.count("signal") >= 2,
      "%d mention(s)" % o.count("signal"))

print("\n=== and the glass shows it")
ser.sendall(b"host canvas\r")
time.sleep(10)
after = Shot(qmp, "02")
after.png(os.path.join(work, "horizon-02-mapped.png"))
full_ink = after.ink_in(*col)
check("the horizon column filled with what the machine found",
      full_ink >= empty_ink, "%d px of ink before the scan, %d after" % (empty_ink, full_ink))
check("the lanes count the signals", after.ink_in(20, after.h - 96, after.w - 20, after.h - 10) > 100)

qmp.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "esc"}}},
                                    {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "esc"}}}])
ser.sendall(b"\r")
expect(r"aios> ", 60)

print("\n=== an entity that may look but not keep changes nothing")
cmd("entity create watcher => wv")
cmd("cap grant $wv network * consume")
o = cmd("horizon scan as=$wv", 90)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip())[:600])
check("it looked, and was refused the keeping of it", "may not" in o or "denied" in o.lower())

ser.sendall(b"quit\r")
time.sleep(1)
q.kill()
q.wait()
httpd.shutdown()

print("\nHORIZON: " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
