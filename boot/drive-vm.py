#!/usr/bin/env python3
"""Drive the installable media in a VM, end to end, over the serial console.

  boot/drive-vm.py <media-dir> [--uefi]

1. boot the ISO (BIOS via isolinux, or UEFI via OVMF + systemd-boot), pick the
   serial-console entry, reach the AIOS prompt, `host install` onto a blank
   virtual disk;
2. boot from that disk, create state, `host reboot`;
3. boot from the disk again and prove the state and kernel identity survived.
No KVM required (TCG); slower, but real.
"""
import os, pty, re, select, subprocess, sys, time

MEDIA = os.path.abspath(sys.argv[1]); UEFI = "--uefi" in sys.argv
ISO = os.path.join(MEDIA, "aios.iso"); DISK = os.path.join(MEDIA, "vm-disk-%s.img" % ("uefi" if "--uefi" in sys.argv else "bios"))
OVMF = "/usr/share/ovmf/OVMF.fd"
LOG = open(os.path.join(MEDIA, "drive-vm-%s.log" % ("uefi" if UEFI else "bios")), "w")

class VM:
    def __init__(self, cdrom):
        cmd = ["qemu-system-x86_64", "-m", "1024", "-smp", "2", "-nographic", "-no-reboot",
               "-drive", "file=%s,format=raw,if=virtio" % DISK]
        if os.path.exists("/dev/kvm"): cmd += ["-enable-kvm"]
        if UEFI: cmd += ["-bios", OVMF]
        if cdrom: cmd += ["-cdrom", ISO, "-boot", "d"]
        else: cmd += ["-boot", "c"]
        self.master, slave = pty.openpty()
        self.p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        self.buf = b""
    def expect(self, pattern, timeout=300):
        rx = re.compile(pattern.encode()); t0 = time.time()
        while time.time() - t0 < timeout:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]; self.buf = self.buf[m.end():]
                return out.decode(errors="replace")
            r, _, _ = select.select([self.master], [], [], 1)
            if r:
                try: d = os.read(self.master, 65536)
                except OSError: d = b""
                if not d:
                    if self.p.poll() is not None: break
                    continue
                LOG.write(d.decode(errors="replace")); LOG.flush(); self.buf += d
        raise SystemExit("TIMEOUT waiting for %r; tail:\n%s" % (pattern, self.buf[-3000:].decode(errors="replace")))
    def send(self, s): os.write(self.master, s.encode())
    def cmd(self, line, timeout=120):
        self.send(line + "\r"); return self.expect(r"\naios> ", timeout)
    def kill(self):
        try: self.p.kill()
        except Exception: pass
        self.p.wait(); os.close(self.master)

def pick_serial_entry(vm, cdrom=True):
    if cdrom:
        # grub menu over serial (both firmwares): down arrow to the serial-console entry, enter.
        vm.expect(r"console on serial port", 300); time.sleep(1.0); vm.send("\x1b[B"); time.sleep(0.5); vm.send("\r")
    elif UEFI:
        # systemd-boot menu on the installed disk over the firmware console: down arrow, enter.
        vm.expect("AIOS", 240); time.sleep(1.5); vm.send("\x1b[B"); time.sleep(0.5); vm.send("\r")
    else:
        # the installed disk: gptmbr → syslinux VBR → plain `boot:` prompt; type the label.
        vm.expect(r"boot:", 240); vm.send("serial\r")

def step(title): print("\n=== " + title, flush=True)

subprocess.run(["rm", "-f", DISK]); subprocess.run(["truncate", "-s", "4G", DISK], check=True)
step("boot ISO (%s) and install" % ("UEFI/OVMF" if UEFI else "BIOS/grub"))
vm = VM(cdrom=True); pick_serial_entry(vm)
vm.expect(r"aios> ", 600); print("reached the AIOS prompt from the ISO")
out = vm.cmd("host disks"); print(out)
assert "/dev/vda" in out and "boot media" in out, "disk listing"
out = vm.cmd("info"); live_kernel = re.search(r"kernel\s+([0-9a-f]{32})", out).group(1); print("live kernel identity", live_kernel)
print(vm.cmd("entity create born-on-media => b"))
out = vm.cmd("host install /dev/vda", 900); print(out)
assert "installed." in out, "install did not report success"
vm.send("host poweroff\r"); time.sleep(3); vm.kill()

step("boot from the installed disk")
vm = VM(cdrom=False); pick_serial_entry(vm, cdrom=False)
vm.expect(r"aios> ", 600); print("reached the AIOS prompt from the disk")
out = vm.cmd("info"); disk_kernel = re.search(r"kernel\s+([0-9a-f]{32})", out).group(1); print(out)
assert disk_kernel == live_kernel, "kernel identity did not carry from the live session onto the disk"
out = vm.cmd("host disks"); assert "AIOS root" in out, out; print(out)
print(vm.cmd("entity create survivor => s"))
print(vm.cmd("cap grant $s storage * read,write"))
out = vm.cmd('store put $s "state written before the reboot" origin=first-disk-boot'); print(out)
assert "object " in out, "store put failed"
vm.cmd("mem alloc kernel 4096 persistent")
vm.send("host reboot\r"); time.sleep(4); vm.kill()

step("boot from the disk again: identity and state must be there")
vm = VM(cdrom=False); pick_serial_entry(vm, cdrom=False)
vm.expect(r"aios> ", 600)
out = vm.cmd("info"); again = re.search(r"kernel\s+([0-9a-f]{32})", out).group(1); print(out)
assert again == disk_kernel, "kernel identity changed across reboot"
out = vm.cmd("store list"); print(out)
assert "state written before the reboot" in out and "origin=first-disk-boot" in out, "object store did not persist"
vm.send("host poweroff\r"); time.sleep(3); vm.kill()
print("\nPROVEN: ISO boots, installs, disk boots, identity %s and stored state survive reboot (%s)" % (again[:12], "UEFI" if UEFI else "BIOS"))
