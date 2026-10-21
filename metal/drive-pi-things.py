#!/usr/bin/env python3
"""Things on the Pi's kernel, under QEMU raspi3b: boot the arm64 image, open
the dashboard Terminal (F4), type SQL through grain's sql() one line at a time
on an emulated USB keyboard (serial input does not reach the kernel on
raspi3b), and read what the kernel printed - its fault handler included - from
the serial log.

  metal/drive-pi-things.py [--keep] [--lines FILE] [--keep-disk]
"""
import json, os, socket, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
kernel = os.path.join(root, "build/metal-arm64/aios-metal-arm64.bin")
disk = "/tmp/pi-things-disk.img"
qmp_path = "/tmp/pi-things-q.sock"
serial = "/tmp/pi-things-serial.log"

# (the board's terminal keeps ~128 characters of a line)
LINES = [
    'grain print(sql("SELECT 1 AS one"))',
    'grain print(sql("CREATE TABLE tw (id INT PRIMARY KEY, w VARCHAR(5), a INT)"))',
    'grain print(sql("INSERT INTO tw VALUES (1, \'pi\', 5), (2, \'pi\', 2), (3, \'sv\', 1)"))',
    'grain print(sql("SELECT w, SUM(a) AS s FROM tw GROUP BY w"))',
    'grain print(sql("SELECT id, w, SUM(a) AS s FROM tw GROUP BY id, w"))',
    'grain print(sql("CREATE THING t KEY w PARTITION BY id READS tw AS SELECT w, SUM(a) AS s FROM tw GROUP BY w"))',
    'grain print(sql("SHOW THINGS t"))',
    'grain print(sql("INSERT INTO tw VALUES (4, \'pi\', 10)"))',
    'grain print(sql("SHOW THINGS t"))',
    'grain print(sql("SHOW THING HISTORY t \'pi\'"))',
]

SHIFTED = {'(': '9', ')': '0', '{': 'bracket_left', '}': 'bracket_right', '"': 'apostrophe', '+': 'equal',
           '>': 'dot', '<': 'comma', '_': 'minus', '*': '8', ':': 'semicolon', '!': '1', '@': '2', '%': '5'}
PLAIN = {' ': 'spc', "'": 'apostrophe', ',': 'comma', '.': 'dot', '=': 'equal', '-': 'minus', ';': 'semicolon',
         '/': 'slash', '[': 'bracket_left', ']': 'bracket_right'}


class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def ev(self, qcode, down):
        self.cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])
        # the board polls its HID keyboard; faster than its poll, keys merge or vanish
        time.sleep(0.06)

    def key(self, qcode, shift=False):
        if shift:
            self.ev("shift", True)
        self.ev(qcode, True)
        self.ev(qcode, False)
        if shift:
            self.ev("shift", False)

    def type(self, text):
        for ch in text:
            if ch.isalpha():
                self.key(ch.lower(), ch.isupper())
            elif ch.isdigit():
                self.key(ch)
            elif ch in SHIFTED:
                self.key(SHIFTED[ch], True)
            elif ch in PLAIN:
                self.key(PLAIN[ch])
            else:
                raise SystemExit("no key for %r" % ch)


def serial_text():
    try:
        return open(serial, errors="replace").read()
    except OSError:
        return ""


def wait_for(needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in serial_text():
            return True
        time.sleep(0.5)
    return False


# --lines FILE: the commands to type (one per line); --keep-disk: boot on the
# card the previous run left (to see what survived a reboot).
if "--lines" in sys.argv:
    LINES = [l.rstrip("\n") for l in open(sys.argv[sys.argv.index("--lines") + 1]) if l.strip()]
# A FAT32 card, as the board's (the build's disk image is not one).
if "--keep-disk" not in sys.argv or not os.path.exists(disk):
    if os.path.exists(disk):
        os.remove(disk)
    subprocess.run(["mkfs.fat", "-F", "32", "-C", disk, str(64 * 1024)], check=True, stdout=subprocess.DEVNULL)
for p in (qmp_path, serial):
    if os.path.exists(p):
        os.remove(p)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel,
                      "-drive", "file=%s,if=sd,format=raw" % disk, "-device", "usb-kbd",
                      "-display", "none", "-qmp", "unix:%s,server,nowait" % qmp_path,
                      "-serial", "file:%s" % serial], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    qmp = QMP(qmp_path)
    if not wait_for("[wdog]", 180):
        print("never reached the desktop loop; serial tail:\n" + serial_text()[-2000:])
        sys.exit(1)
    time.sleep(3)
    qmp.key("f4")
    time.sleep(1)
    for line in LINES:
        before = len(serial_text())
        qmp.type(line)
        qmp.key("ret")
        t0 = time.time()
        while time.time() - t0 < 60:
            time.sleep(1)
            out = serial_text()[before:]
            if "[{" in out or "[]" in out or "affected" in out or "error" in out.lower() or "created" in out.lower() or "{" in out or "fault" in out.lower() or "panic" in out.lower() or q.poll() is not None:
                break
        out = serial_text()[before:]
        print("> " + line)
        print("\n".join("  " + l for l in out.strip().splitlines()[-12:]) or "  (nothing printed)")
        if q.poll() is not None:
            print("QEMU exited")
            break
finally:
    if "--keep" not in sys.argv:
        q.kill()
