#!/usr/bin/env python3
"""Boot the deployable disk image (no CD): first boot formats the AOS partition,
state is written, power off, boot again → same identity and state. --uefi uses OVMF."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args, has_bios_grub, ovmf
import shutil
IMG = sys.argv[1]; UEFI = "--uefi" in sys.argv
if not UEFI and not has_bios_grub():
    print("SKIP: this host has no grub i386-pc modules, so the disk image is UEFI-only (install grub-pc-bin for BIOS coverage)")
    sys.exit(0)
WORK = IMG + (".uefi" if UEFI else ".bios") + ".img"; shutil.copyfile(IMG, WORK)
class VM:
    def __init__(self):
        cmd = ["qemu-system-x86_64", "-m", "512", "-nographic", "-no-reboot", "-drive", "file=%s,if=virtio,format=raw" % WORK, "-boot", "c", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        if UEFI: cmd += firmware_args(force_uefi=True)
        self.master, slave = pty.openpty(); self.p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True); os.close(slave); self.buf = b""
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
vm = VM(); banner = vm.expect(r"aios> ", 240)
check("first boot formats the AOS partition", "formatted on first boot" in banner)
check("volume mounted", "mounted" in banner)
o = vm.cmd("host disks"); print(o); check("disk shows the AOS partition", "AOS partition" in o)
o = vm.cmd("info"); clean_o = re.sub(r'\x1b\[[0-9;=]*[a-zA-Z]', '', o); kid = re.search(r"(?:kernel\s+|Kernel ID\s*:\s*)([0-9a-f]{32})", clean_o, re.IGNORECASE).group(1)
o = vm.cmd("entity create resident => r"); o = vm.cmd("cap grant $r storage * read,write")
o = vm.cmd('store put $r "installed on the disk image" firmware=%s' % ("uefi" if UEFI else "bios")); check("object stored", "object " in o)
vm.off()
print("=== second boot")
vm = VM(); banner = vm.expect(r"aios> ", 240)
check("no reformat on second boot", "formatted on first boot" not in banner and "mounted" in banner)
o = vm.cmd("info"); clean_o2 = re.sub(r'\x1b\[[0-9;=]*[a-zA-Z]', '', o); kid2 = re.search(r"(?:kernel\s+|Kernel ID\s*:\s*)([0-9a-f]{32})", clean_o2, re.IGNORECASE).group(1)
check("same kernel identity (%s)" % kid2[:12], kid2 == kid)
o = vm.cmd("store list"); print(o); check("state survived the power cycle", "installed on the disk image" in o)
vm.off()
bad = [n for n, ok in checks if not ok]
print("DISK(%s): %s" % ("UEFI" if UEFI else "BIOS", "PASS" if not bad else "FAIL")); sys.exit(0 if not bad else 1)
