#!/usr/bin/env python3
"""The wall clock on x86-64 bare metal: the CMOS RTC read at boot, carried by
the TSC. Boots the image under QEMU (whose RTC holds UTC), then checks `date`,
ASQL's NOW() and a new table's CREATE_TIME against the host's own clock.

    metal/drive-clock.py build/metal/aios-metal.iso [cpus]
"""
import atexit, calendar, os, pty, re, select, signal, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args
ISO = sys.argv[1]
CPUS = sys.argv[2] if len(sys.argv) > 2 else "1"
DISK = "/tmp/aios-clock-disk.img"
subprocess.run(["rm", "-f", DISK]); subprocess.run(["truncate", "-s", "64M", DISK], check=True)
master, slave = pty.openpty()
p = subprocess.Popen(["qemu-system-x86_64", "-m", "1024", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"] + firmware_args() +
                     ["-drive", "file=%s,if=virtio,format=raw" % DISK, "-rtc", "base=utc", "-smp", CPUS], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)
# The VM goes with this script however it ends, a timeout's signal included.
atexit.register(lambda: p.poll() is None and p.kill())
for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(sig, lambda *_: sys.exit(1))
buf = b""
def expect(pat, to=240):
    global buf
    rx = re.compile(pat.encode()); t0 = time.time()
    while time.time() - t0 < to:
        m = rx.search(buf)
        if m:
            out = buf[:m.end()]; buf = buf[m.end():]; return out.decode(errors="replace")
        r, _, _ = select.select([master], [], [], 1)
        if r:
            try: d = os.read(master, 65536)
            except OSError: d = b""
            if not d and p.poll() is not None: break
            buf += d
    raise SystemExit("TIMEOUT %r; tail: %s" % (pat, buf[-1500:].decode(errors="replace")))
def cmd(line, to=120):
    os.write(master, (line + "\r").encode()); return expect(r"\naios> ", to)
checks = []
def check(name, ok, detail=""):
    checks.append(ok); print(("ok    " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
def stamps(text):
    return [calendar.timegm(time.strptime(m, "%Y-%m-%d %H:%M:%S")) for m in re.findall(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", text)]
boot = expect(r"aios> ")
m = re.search(r"cpus online: (\d+)", boot)
print("cpus online:", m.group(1) if m else "?")
host_now = time.time()
o = cmd("date"); print(o.strip())
d = stamps(o)
check("date is the host's UTC time", bool(d) and abs(d[0] - host_now) < 120, "skew %ss" % (int(d[0] - host_now) if d else "?"))
cmd("host volume format")
o = cmd("sql query SELECT NOW()"); print(o.strip())
n = stamps(o)
check("NOW() is the host's UTC time", bool(n) and abs(n[0] - time.time()) < 120, "skew %ss" % (int(n[0] - time.time()) if n else "?"))
cmd("sql query CREATE TABLE clock_probe (id INT)")
o = cmd("sql query SELECT TABLE_NAME, CREATE_TIME FROM information_schema.tables"); print(o.strip())
c = [s for line in o.splitlines() if "clock_probe" in line for s in stamps(line)]
check("CREATE_TIME is when the table was made", bool(c) and abs(c[0] - time.time()) < 120, "skew %ss" % (int(c[0] - time.time()) if c else "?"))
os.write(master, b"host poweroff\r"); time.sleep(2); p.kill(); p.wait()
print("%d/%d" % (sum(checks), len(checks)))
sys.exit(0 if all(checks) else 1)
