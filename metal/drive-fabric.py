#!/usr/bin/env python3
"""Two bare-metal kernels, no Linux, on a shared virtual Ethernet segment: B
serves, A attaches over the AOS fabric and computes on B's device, holds memory
there, and publishes state into a channel living on B."""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args, has_bios_grub, ovmf
import socket
ISO = sys.argv[1]
_s = socket.socket(); _s.bind(("127.0.0.1", 0)); PORT = _s.getsockname()[1]; _s.close()
ALL = []
import atexit
def _cleanup():
    for vm in list(ALL):
        try: vm.p.kill(); vm.p.wait()
        except Exception: pass
atexit.register(_cleanup)
class VM:
    def __init__(self, tag, net):
        ALL.append(self)
        cmd = ["qemu-system-x86_64", "-m", "512", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"] + firmware_args() + ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-netdev", net, "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:aa:bb:%s" % tag]
        self.mon = "/tmp/fabric-mon-%s.sock" % tag
        try: os.remove(self.mon)
        except OSError: pass
        cmd += ["-monitor", "unix:%s,server,nowait" % self.mon]
        self.master, slave = pty.openpty(); self.p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True); os.close(slave); self.buf = b""; self.tag = tag
    def expect(self, pat, timeout=180):
        rx = re.compile(pat.encode()); t0 = time.time()
        while time.time() - t0 < timeout:
            m = rx.search(self.buf)
            if m: out = self.buf[:m.end()]; self.buf = self.buf[m.end():]; return out.decode(errors="replace")
            r, _, _ = select.select([self.master], [], [], 0.2)
            if r:
                try: d = os.read(self.master, 65536)
                except OSError: d = b""
                if not d and self.p.poll() is not None: break
                self.buf += d
        raise SystemExit("[%s] TIMEOUT waiting for %r; tail: %s" % (self.tag, pat, self.buf[-2500:].decode(errors="replace")))
    def cmd(self, line, timeout=120): os.write(self.master, (line + "\r").encode()); return self.expect(r"\naios> ", timeout)
    def off(self): os.write(self.master, b"host poweroff\r"); time.sleep(1); self.p.kill(); self.p.wait()
checks = []
def check(name, ok): checks.append((name, ok)); print(("ok    " if ok else "FAIL  ") + name, flush=True)
B = VM("02", "socket,id=n0,listen=127.0.0.1:%d" % PORT); B.expect(r"aios> ", 240)
A = VM("01", "socket,id=n0,connect=127.0.0.1:%d" % PORT); A.expect(r"aios> ", 240)
o = B.cmd("host net"); print(o); check("B has a nic on the fabric", "mac 52:54:00:aa:bb:02" in o)
o = B.cmd("node serve %d lab-shared-secret" % 47311); print(o); check("B serves", "serving" in o)
o = A.cmd("entity create traveller => t"); A.cmd("cap grant $t kernel * schedule"); A.cmd("cap grant $t compute * execute"); A.cmd("cap grant $t network * publish")
o = A.cmd("node connect 52:54:00:aa:bb:02 47311 lab-shared-secret", 90); print(o); check("A attached B over the fabric (encrypted link)", "attached node" in o)
def where_is(vm):
    import socket as sk
    try:
        c = sk.socket(sk.AF_UNIX); c.settimeout(5); c.connect(vm.mon); time.sleep(0.3); c.recv(4096)
        c.sendall(b"info registers\n"); time.sleep(0.5); data = c.recv(65536).decode(errors="replace"); c.close()
        m = re.search(r"RIP=([0-9a-f]+)", data); rsp = re.search(r"RSP=([0-9a-f]+)", data)
        if not m: return "no RIP in monitor output"
        rip = int(m.group(1), 16)
        sym = subprocess.run(["bash", "-c", "nm -n build/metal/aios-metal.elf | awk '$1 <= \"%016x\" {last=$0} END {print last}'" % rip], capture_output=True, text=True).stdout.strip()
        return "RIP=%x RSP=%s in %s" % (rip, rsp.group(1) if rsp else "?", sym)
    except Exception as e: return "monitor failed: %s" % e
def probe_fail(where):
    print("--- where is B:", where_is(B)); time.sleep(1); print("--- where is B (again):", where_is(B))
    print("--- where is A:", where_is(A))
    print("--- STALL at %s; A exit=%s B exit=%s" % (where, A.p.poll(), B.p.poll()))
    print("--- B raw tail:", B.buf[-600:].decode(errors="replace"))
    print("--- B's view:")
    try: print(B.cmd("host net", 90)); print(B.cmd("node peers", 30))
    except SystemExit as e: print(e)
    for vm in ALL:
        try: vm.p.kill()
        except Exception: pass
    print("FABRIC: FAIL"); sys.exit(1)
print("--- B sessions after attach:"); print(B.cmd("host net", 30))
print("--- A sessions after attach:"); print(A.cmd("host net", 30))
try: o = A.cmd("mem alloc $t 64 remote => rm0", 150)
except SystemExit as e: print(e); probe_fail("first remote request (mem alloc)")
if "memory " not in o:
    print("--- request failed on A:", o.strip().splitlines()[-1] if o.strip() else o)
    print("--- A fabric:"); print(A.cmd("host net", 60))
    print("--- B fabric:"); print(B.cmd("host net", 60))
    sys.exit(1)
check("a single remote request completes", "memory " in o); print(o)
try: o = A.cmd("host net", 20); print(o)
except SystemExit as e: print(e); probe_fail("host net on A")
A.cmd("tensor create $t f32 2x3 1 2 3 4 5 6 => X"); A.cmd("tensor create $t f32 3x2 7 8 9 10 11 12 => Y"); A.cmd("tensor create $t f32 2x2 => Z")
A.cmd("graph create $t => g"); A.cmd("graph op $t $g matmul in=$X:f32:2x3 in=$Y:f32:3x2 out=$Z:f32:2x2 dev=remote-cpu")
try: o = A.cmd("graph run $t $g", 60)
except SystemExit as e: print(e); probe_fail("graph run")
print(o); check("graph ran on B's device", "remote:" in o and "complete" in o)
o = A.cmd("tensor show $t $Z f32 2x2"); print(o); check("result came back over the fabric [58 64 139 154]", "[58 64 139 154]" in o)
o = A.cmd("mem alloc $t 64 remote => rm"); A.cmd('mem write $t $rm 0 "bytes held on node B, no Linux anywhere"'); o = A.cmd("mem read $t $rm 0 39"); check("remote memory round trip", "bytes held on node B" in o)
o = A.cmd("channel create $t 8 512 transport=$transport => ch"); A.cmd('state create $t working "published across the fabric" => st'); o = A.cmd("channel publish $t $ch $st"); check("published into a channel on B", "published frame" in o)
o = B.cmd("node peers"); print(o); check("B sees the guest", "guest" in o)
o = B.cmd("set guest"); o = B.cmd("channel find $guest"); o = B.cmd("set ch"); B.cmd("entity create reader => r"); B.cmd("channel admit $guest $ch $r consume"); B.cmd("channel subscribe $r $ch")
o = B.cmd("channel consume $r $ch"); print(o); check("B's entity consumed A's state", "published across the fabric" in o)
o = A.cmd("host net"); print(o)
A.off(); B.off()
bad = [n for n, ok in checks if not ok]
print("FABRIC: " + ("PASS" if not bad else "FAIL")); sys.exit(0 if not bad else 1)
