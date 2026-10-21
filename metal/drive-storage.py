#!/usr/bin/env python3
"""Bare metal, no Linux: claim a virtio disk as an AOS volume, write state, power
off, boot again from the same disk, and find the same kernel identity and state."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args, has_bios_grub, ovmf
ISO = sys.argv[1]; DISK = sys.argv[2] if len(sys.argv) > 2 else "/tmp/aios-metal-disk.img"
subprocess.run(["rm", "-f", DISK]); subprocess.run(["truncate", "-s", "64M", DISK], check=True)
class VM:
    def __init__(self):
        self.master, slave = pty.openpty()
        self.p = subprocess.Popen(["qemu-system-x86_64", "-m", "512", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"] + firmware_args() + ["-drive", "file=%s,if=virtio,format=raw" % DISK, "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave); self.buf = b""
    def expect(self, pat, timeout=180):
        rx = re.compile(pat.encode()); t0 = time.time()
        while time.time() - t0 < timeout:
            m = rx.search(self.buf)
            if m: out = self.buf[:m.end()]; self.buf = self.buf[m.end():]; return out.decode(errors="replace")
            r, _, _ = select.select([self.master], [], [], 1)
            if r:
                try: d = os.read(self.master, 65536)
                except OSError: d = b""
                if not d and self.p.poll() is not None: break
                self.buf += d
        raise SystemExit("TIMEOUT waiting for %r; tail: %s" % (pat, self.buf[-2500:].decode(errors="replace")))
    def cmd(self, line): os.write(self.master, (line + "\r").encode()); return self.expect(r"\naios> ")
    def off(self): os.write(self.master, b"host poweroff\r"); time.sleep(2); self.p.kill(); self.p.wait()
checks = []
def check(name, ok): checks.append((name, ok)); print(("ok    " if ok else "FAIL  ") + name)
vm = VM(); banner = vm.expect(r"aios> ", 180)
check("boot sees the virtio disk with no volume", "no AOS volume" in banner)
o = vm.cmd("host disks"); print(o)
o = vm.cmd("host volume format"); print(o); check("volume formatted and adopted", "formatted" in o)
o = vm.cmd("info"); clean_o = re.sub(r'\x1b\[[0-9;=]*[a-zA-Z]', '', o); kid = re.search(r"(?:kernel\s+|Kernel ID\s*:\s*)([0-9a-f]{32})", clean_o, re.IGNORECASE).group(1)
o = vm.cmd("entity create keeper => k"); o = vm.cmd("cap grant $k storage * read,write")
o = vm.cmd('store put $k "kept on the volume" origin=bare-metal'); check("object stored", "object " in o)
o = vm.cmd("mem alloc $k 4096 persistent => pm"); o = vm.cmd('mem write $k $pm 0 "persistent bytes on the metal"'); check("persistent memory written", "wrote" in o)
o = vm.cmd("host volume"); print(o)
vm.off()
print("=== second boot from the same disk")
vm = VM(); banner = vm.expect(r"aios> ", 180)
check("volume mounted at boot", "mounted" in banner)
o = vm.cmd("info"); clean_o2 = re.sub(r'\x1b\[[0-9;=]*[a-zA-Z]', '', o); kid2 = re.search(r"(?:kernel\s+|Kernel ID\s*:\s*)([0-9a-f]{32})", clean_o2, re.IGNORECASE).group(1); print(o)
check("same kernel identity after power cycle (%s)" % kid2[:12], kid2 == kid)
o = vm.cmd("store list"); print(o); check("stored object survived the power cycle", "kept on the volume" in o and "origin=bare-metal" in o)
o = vm.cmd("host volume"); print(o); check("volume reports records", "records" in o)
vm.off()
bad = [n for n, ok in checks if not ok]
print("STORAGE: " + ("PASS" if not bad else "FAIL")); sys.exit(0 if not bad else 1)
