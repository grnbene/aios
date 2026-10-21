#!/usr/bin/env python3
"""Drive the bare-metal command surface over the serial console in QEMU:
boot the interactive ISO, reach `aios>`, run commands, check what came back."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args, has_bios_grub, ovmf
ISO = sys.argv[1]
master, slave = pty.openpty()
p = subprocess.Popen(["qemu-system-x86_64", "-m", "512", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"] + firmware_args() + ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave); buf = b""
def expect(pat, timeout=120):
    global buf; rx = re.compile(pat.encode()); t0 = time.time()
    while time.time() - t0 < timeout:
        m = rx.search(buf)
        if m: out = buf[:m.end()]; buf = buf[m.end():]; return out.decode(errors="replace")
        r, _, _ = select.select([master], [], [], 1)
        if r:
            try: d = os.read(master, 65536)
            except OSError: d = b""
            if not d and p.poll() is not None: break
            buf += d
    raise SystemExit("TIMEOUT waiting for %r; tail: %s" % (pat, buf[-2000:].decode(errors="replace")))
def cmd(line): os.write(master, (line + "\r").encode()); return expect(r"\naios> ")
expect(r"aios> ", 180); print("reached the bare-metal prompt")
checks = []
o = cmd("info"); checks.append(("info shows bare-metal host", "bare-metal" in o)); print(o)
o = cmd("entity create pilot => e"); checks.append(("entity created", "entity " in o))
o = cmd("cap grant $e kernel * schedule"); o = cmd("cap grant $e compute * execute")
o = cmd("tensor create $e f32 2x2 1 2 3 4 => A"); o = cmd("tensor create $e f32 2x2 5 6 7 8 => B"); o = cmd("tensor create $e f32 2x2 => C")
o = cmd("graph create $e => g"); o = cmd("graph op $e $g matmul in=$A:f32:2x2 in=$B:f32:2x2 out=$C:f32:2x2")
o = cmd("graph run $e $g"); checks.append(("graph ran on the CPU device", "complete" in o)); print(o)
o = cmd("tensor show $e $C f32 2x2"); checks.append(("matmul [19 22 43 50]", "[19 22 43 50]" in o)); print(o)
o = cmd("state create $e context \"typed on the metal\""); o = cmd("mind bind $e echo")
o = cmd("mind cycle $e"); checks.append(("cognitive cycle from the prompt", "typed on the metal" in o)); print(o)
o = cmd("! mem read $e 00000000000000000000000000000000 0 4"); checks.append(("refusal path works (exceptions in the command surface)", "refused as expected" in o)); print(o)
o = cmd("host heap"); checks.append(("host tool answers", "heap in use" in o)); print(o)
os.write(master, b"quit\r"); time.sleep(2); p.kill(); p.wait()
bad = [n for n, ok in checks if not ok]
for n, ok in checks: print(("ok    " if ok else "FAIL  ") + n)
print("SHELL: " + ("PASS" if not bad else "FAIL"))
sys.exit(0 if not bad else 1)
