#!/usr/bin/env python3
"""Boot the bare-metal image and make it parse real numbers.

The metal link had been broken since the SQL engine landed (__isoc23_strtoll and
strtod are not in a freestanding environment), and the one double-parser that WAS
here returned 0.0 for everything. So this does not check that it links: it checks
that the machine, with no Linux underneath it, gets the right answer for a number
with a fraction and an exponent in it."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, "os.path.dirname(os.path.abspath(__file__))")
from qemu_common import firmware_args
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")
master, slave = pty.openpty()
p = subprocess.Popen(["qemu-system-x86_64","-m","512","-nographic","-no-reboot","-cdrom",ISO,"-boot","d"]
                     + firmware_args() + ["-device","isa-debug-exit,iobase=0xf4,iosize=0x04"],
                     stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave); buf=b""
def expect(pat, timeout=180):
    global buf; rx=re.compile(pat.encode()); t0=time.time()
    while time.time()-t0 < timeout:
        m=rx.search(buf)
        if m: out=buf[:m.end()]; buf=buf[m.end():]; return out.decode(errors="replace")
        r,_,_=select.select([master],[],[],1)
        if r:
            try: d=os.read(master,65536)
            except OSError: d=b""
            if not d and p.poll() is not None: break
            buf+=d
    raise SystemExit("TIMEOUT waiting for %r; tail:\n%s" % (pat, buf[-3000:].decode(errors="replace")))
def cmd(line):
    os.write(master,(line+"\r").encode()); return expect(r"\naios> ")
expect(r"aios> ", 240); print("reached the bare-metal prompt\n")
checks=[]
o=cmd("info"); checks.append(("runs with no Linux underneath","bare-metal" in o))
o=cmd("sql exec \"CREATE TABLE m (id INT, ratio DOUBLE, label TEXT)\""); print(o)
o=cmd("sql exec \"INSERT INTO m VALUES (1, 3.25, 'a'), (2, 1.5e2, 'b'), (3, 0.125, 'c')\""); print(o)
o=cmd("sql query \"SELECT id, ratio FROM m ORDER BY ratio DESC\""); print(o)
checks.append(("3.25 parsed, not truncated to 3 or zeroed","3.25" in o))
checks.append(("1.5e2 parsed as 150","150" in o))
checks.append(("0.125 parsed","0.125" in o))
o=cmd("sql query \"SELECT SUM(ratio) FROM m\""); print(o)
checks.append(("sum is 153.375 — arithmetic on parsed doubles","153.375" in o))
o=cmd("sql query \"SELECT id FROM m WHERE ratio > 1.0 AND ratio < 100.0\""); print(o)
# 3.25 is the only value between 1 and 100, so the answer is the single row id=1.
rows = [l for l in o.splitlines() if l.startswith("| ") and "id" not in l]
checks.append(("a fractional WHERE bound selects exactly the one row",
               len(rows) == 1 and rows[0].split("|")[1].strip() == "1"))
print()
bad=0
for name, ok in checks:
    print(("  PASS  " if ok else "  FAIL  ")+name)
    bad += 0 if ok else 1
os.write(master,b"quit\r"); time.sleep(2); p.kill()
print("\n%d check(s), %d failed" % (len(checks), bad))
sys.exit(1 if bad else 0)
