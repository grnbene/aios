#!/usr/bin/env python3
"""Boot the bare-metal image and make it train and use its OWN language model.

There is no Linux under this, no BLAS, no inference server and no store: the
tokenizer is learned at boot, the transformer is trained at boot, and the
machine answers a plain-English question from weights it computed itself."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")
master, slave = pty.openpty()
p = subprocess.Popen(["qemu-system-x86_64","-m","1024","-nographic","-no-reboot","-cdrom",ISO,"-boot","d"]
                     + firmware_args() + ["-device","isa-debug-exit,iobase=0xf4,iosize=0x04"],
                     stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave); buf=b""
def expect(pat, timeout=300):
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
def cmd(line, timeout=300):
    os.write(master,(line+"\r").encode()); return expect(r"\naios> ", timeout)
expect(r"aios> ", 300); print("reached the bare-metal prompt\n")
checks=[]
o=cmd("info"); checks.append(("runs with no Linux underneath","bare-metal" in o))
o=cmd("lm status"); print(o)
checks.append(("the model built itself here", "trained here" in o or "loaded" in o))
checks.append(("it learned a vocabulary over the 256 bytes", "learned merges" in o))
checks.append(("it says plainly it is not a general assistant", "not a general assistant" in o))
o=cmd("lm check 6", 600); print(o)
checks.append(("the hand-written backward pass agrees with the function it differentiates",
               "the derivatives agree" in o))
o=cmd("lm train 120", 900); print(o)
m=re.search(r"loss ([0-9.]+) -> ([0-9.]+)", o)
if m:
    before, after = float(m.group(1)), float(m.group(2))
    print("  loss %.4f -> %.4f" % (before, after))
    checks.append(("training on bare metal reduced the loss", after < before))
else:
    checks.append(("training on bare metal reduced the loss", False))
o=cmd("lm ask what may each entity do", 600); print(o)
checks.append(("it mapped a plain English request onto a command", "authority" in o))
checks.append(("it answered from the model, not the keyword chain", "the model" in o))
print()
bad=0
for name, ok in checks:
    print(("  PASS  " if ok else "  FAIL  ")+name); bad += 0 if ok else 1
os.write(master,b"quit\r"); time.sleep(2); p.kill()
print("\n%d check(s), %d failed" % (len(checks), bad))
sys.exit(1 if bad else 0)
