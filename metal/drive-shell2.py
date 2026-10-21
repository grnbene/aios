#!/usr/bin/env python3
"""The familiar shell on bare metal, persisting on the AOS volume across a reboot."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args, has_bios_grub, ovmf
ISO = sys.argv[1]; DISK = sys.argv[2] if len(sys.argv) > 2 else "/tmp/aios-shell-disk.img"
subprocess.run(["rm", "-f", DISK]); subprocess.run(["truncate", "-s", "64M", DISK], check=True)
class VM:
    def __init__(self):
        self.master, slave = pty.openpty()
        self.p = subprocess.Popen(["qemu-system-x86_64","-m","512","-nographic","-no-reboot","-cdrom",ISO,"-boot","d"] + firmware_args() + ["-drive","file=%s,if=virtio,format=raw"%DISK,"-device","isa-debug-exit,iobase=0xf4,iosize=0x04"], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave); self.buf=b""
    def expect(self,pat,to=180):
        rx=re.compile(pat.encode()); t0=time.time()
        while time.time()-t0<to:
            m=rx.search(self.buf)
            if m: out=self.buf[:m.end()]; self.buf=self.buf[m.end():]; return out.decode(errors="replace")
            r,_,_=select.select([self.master],[],[],1)
            if r:
                try: d=os.read(self.master,65536)
                except OSError: d=b""
                if not d and self.p.poll() is not None: break
                self.buf+=d
        raise SystemExit("TIMEOUT %r; tail: %s"%(pat,self.buf[-1500:].decode(errors="replace")))
    def cmd(self,l): os.write(self.master,(l+"\r").encode()); return self.expect(r"\naios> ")
    def off(self): os.write(self.master,b"host poweroff\r"); time.sleep(2); self.p.kill(); self.p.wait()
checks=[]
def check(n,ok): checks.append((n,ok)); print(("ok    " if ok else "FAIL  ")+n)
vm=VM(); vm.expect(r"aios> ",240)
vm.cmd("host volume format")
vm.cmd("mkdir -p /work"); vm.cmd("cd /work")
o=vm.cmd("echo an intelligence lived here > log.txt"); o=vm.cmd("echo and wrote a second line >> log.txt")
o=vm.cmd("ls -l"); print(o); check("ls shows the file", "log.txt" in o)
o=vm.cmd("cat log.txt"); check("cat reads it back", "an intelligence lived here" in o)
o=vm.cmd("wc log.txt"); print(o); check("wc counts 2 lines", any(ln.strip().startswith("2 ") or ln.split()[:1]==["2"] for ln in o.splitlines() if "log.txt" in ln and "wc" not in ln.split()[:1]))
o=vm.cmd("grep second log.txt"); check("grep finds the line", "second line" in o)
o=vm.cmd("uname -a"); print(o); check("uname reports AIOS", "AIOS" in o)
o=vm.cmd("ps"); check("ps lists the kernel entity", "kernel" in o)
vm.off()
print("=== reboot")
vm=VM(); vm.expect(r"aios> ",240)
o=vm.cmd("cat /work/log.txt"); print(o); check("file survived the reboot on the AOS volume", "an intelligence lived here" in o and "second line" in o)
o=vm.cmd("rm /work/log.txt"); o=vm.cmd("ls /work"); check("rm works (volume delete record)", "log.txt" not in o)
vm.off()
bad=[n for n,ok in checks if not ok]
print("SHELL2: "+("PASS" if not bad else "FAIL")); sys.exit(0 if not bad else 1)
