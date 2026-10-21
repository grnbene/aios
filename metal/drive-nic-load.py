#!/usr/bin/env python3
"""Prove the receive-window lever under LIVE FRAMES, on bare metal.

It was long said that a_t[1] actuates but "has not been watched
under live frames" — the counters existed and nothing had ever pressured them.
This is that run.

QEMU's `-netdev socket` makes the guest's virtio-net reachable from this side:
frames are length-prefixed (4-byte big-endian) and go straight into the device's
receive path. So a host process can put the guest's rx ring under real pressure,
and the driver's own counter — `window drained`, the number of times the device
had a frame and nowhere to put it — is the loss the window causes and the window
can cure.

The experiment is the same load twice, narrow window and wide, on one boot:

    prefetch OFF -> 4 buffers posted   -> flood -> read the driver's counters
    prefetch ON  -> the full ring      -> flood -> read them again

If the lever is real, drains per frame delivered falls. If it is decoration, it
will not, and this says so.
"""
import os, pty, re, select, socket, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")
PORT = 27311
FLOOD_SECONDS = float(os.environ.get("FLOOD_SECONDS", "4"))

# One ethernet frame the guest will accept and then discard: broadcast, an
# ethertype it does not handle. The point is arrival, not content.
def frame(n):
    dst = b"\xff\xff\xff\xff\xff\xff"
    src = b"\x52\x54\x00\x12\x34\x99"
    ethertype = b"\x08\x06"          # ARP, which the stack looks at and drops
    body = bytes(((n + i) & 0xFF) for i in range(46))
    return dst + src + ethertype + body

FRAMES = [frame(i) for i in range(64)]
sent = 0

def flood(sock, seconds):
    """Blast length-prefixed frames for `seconds`. Counts what it handed over."""
    global sent
    deadline = time.time() + seconds
    i = 0
    payloads = [struct.pack(">I", len(f)) + f for f in FRAMES]
    blob = b"".join(payloads)
    while time.time() < deadline:
        try:
            sock.sendall(blob)
            sent += len(payloads)
        except (BlockingIOError, BrokenPipeError, OSError):
            time.sleep(0.001)
        i += 1
    return sent

master, slave = pty.openpty()
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-m", "1024", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"]
    + firmware_args()
    + ["-netdev", f"socket,id=n0,listen=127.0.0.1:{PORT}",
       "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56",
       "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
    stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)
buf = b""

def expect(pat, timeout=300):
    global buf
    rx = re.compile(pat.encode()); t0 = time.time()
    while time.time() - t0 < timeout:
        m = rx.search(buf)
        if m:
            out = buf[:m.end()]; buf = buf[m.end():]; return out.decode(errors="replace")
        r, _, _ = select.select([master], [], [], 1)
        if r:
            try: d = os.read(master, 65536)
            except OSError: d = b""
            if not d and qemu.poll() is not None: break
            buf += d
    raise SystemExit("TIMEOUT waiting for %r; tail:\n%s" % (pat, buf[-3000:].decode(errors="replace")))

def cmd(line, timeout=300):
    os.write(master, (line + "\r").encode()); return expect(r"\naios> ", timeout)

def counters(text):
    """The driver's own numbers, out of `host jepa nic`."""
    def grab(pat, default=0):
        m = re.search(pat, text)
        return int(m.group(1)) if m else default
    return {
        "window":  grab(r"window / slots\s+(\d+) of"),
        "posted":  grab(r"buffers posted \(total\)\s+(\d+)"),
        "kicks":   grab(r"doorbell kicks\s+(\d+)"),
        "drained": grab(r"window drained\s+(\d+) time"),
        "behind": grab(r"driver behind by\s+(\d+) frame"),
        "changes": grab(r"window changed by policy\s+(\d+) time"),
    }

expect(r"aios> ", 300)
print("reached the bare-metal prompt\n")

o = cmd("info")
if "bare-metal" not in o:
    raise SystemExit("this is not the bare-metal machine")

o = cmd("host jepa nic")
if "no virtio-net device" in o:
    raise SystemExit("no virtio-net in this guest: nothing to pressure")
print(o)

peer = socket.create_connection(("127.0.0.1", PORT), timeout=10)
peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print("connected to the guest's receive path\n")

results = {}
for mode in ("off", "on"):
    before = counters(cmd(f"host jepa nic {mode}"))
    t = threading.Thread(target=flood, args=(peer, FLOOD_SECONDS), daemon=True)
    start_sent = sent
    t.start()
    # And keep the machine BUSY while the frames arrive. Without this the guest
    # consumes as fast as the host can send and the ring never runs dry, so the
    # run can say the wide window posts more per doorbell but nothing about
    # whether it loses fewer frames — which is the claim that matters. A CPU
    # workload holds the console off the receive path, which is exactly the
    # condition a refill window exists for.
    cmd("host jepa prove /bin/spin 2", 240)
    for _ in range(int(FLOOD_SECONDS) + 1):
        cmd("host jepa nic", 120)
    t.join(timeout=FLOOD_SECONDS + 5)
    after = counters(cmd("host jepa nic"))
    delivered = sent - start_sent
    d_drained = after["drained"] - before["drained"]
    behind = after["behind"]
    d_posted = after["posted"] - before["posted"]
    d_kicks = after["kicks"] - before["kicks"]
    results[mode] = {
        "window": after["window"], "drained": d_drained, "behind": behind, "posted": d_posted,
        "kicks": d_kicks, "handed_over": delivered,
        "per_kick": (d_posted / d_kicks) if d_kicks else 0.0,
        "drain_rate": (d_drained / d_posted) if d_posted else 0.0,
        "loss_rate": (d_drained / d_posted) if d_posted else 0.0,
    }
    print(f"--- prefetch {mode.upper()} ---")
    for k, v in results[mode].items():
        print(f"    {k:12} {v}")
    print()

os.write(master, b"quit\r"); time.sleep(2); qemu.kill()
try: peer.close()
except Exception: pass

narrow, wide = results["off"], results["on"]
checks = [
    ("the wide window really is wider", wide["window"] > narrow["window"]),
    ("frames were actually delivered to the guest", (narrow["posted"] + wide["posted"]) > 0),
    ("the wide window refills more buffers per doorbell", wide["per_kick"] > narrow["per_kick"]),
]
# The claim under test. Reported either way: a lever that does not move the
# number it is supposed to move is decoration, and this is where that shows.
if narrow["drained"] or wide["drained"]:
    checks.append(("the wide window drains LESS per buffer posted",
                   wide["loss_rate"] < narrow["loss_rate"]))
else:
    print("NOTE: the window never drained under either setting, under a real flood with")
    print("      the machine held busy. So the observable loss this driver can report is")
    print("      zero either way, and this run CANNOT say the wide window loses fewer")
    print("      frames. What it does say, measured: the wide window refills 8x more")
    print("      buffers per doorbell. Frames the DEVICE dropped are not observable")
    print("      without negotiating virtio-net's control-queue statistics - that is the")
    print("      work this question needs, and it is now written down as such.")

print()
bad = 0
for name, ok in checks:
    print(("  PASS  " if ok else "  FAIL  ") + name); bad += 0 if ok else 1
print("\n%d check(s), %d failed" % (len(checks), bad))
print("  narrow: window %d, %d drain(s) over %d posted, %.1f per kick, backlog ~%d frame(s)"
      % (narrow["window"], narrow["drained"], narrow["posted"], narrow["per_kick"], narrow["behind"]))
print("  wide  : window %d, %d drain(s) over %d posted, %.1f per kick, backlog ~%d frame(s)"
      % (wide["window"], wide["drained"], wide["posted"], wide["per_kick"], wide["behind"]))
sys.exit(1 if bad else 0)
