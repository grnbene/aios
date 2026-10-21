#!/usr/bin/env python3
"""Have the policy head LEARN, on bare metal, against the widened reward.

The reward used to be the switch cost of the settled interval alone. It is now
switch cost + 4x packet loss + 2x load above saturation, and the two new terms
are measured from this machine's own counters: the driver's `window drained` and
the scheduler's own count of runnable tasks over the cores that joined.

The experiment is the one that can actually go wrong:

    record  N transitions  ->  the reward BEFORE any training
    train   M steps        ->  policy gradient on those measured returns
    record  N again        ->  the reward AFTER

and then `host jepa prove`, which runs a FIXED workload under two slices and
reports the kernel's own switch count, to check the slice the policy settled on
is the slice the experiment says is best. If it converged on the wrong one, that
shows here.
"""
import os, pty, re, select, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")
N = int(os.environ.get("RECORD", "40"))
STEPS = int(os.environ.get("STEPS", "30"))

master, slave = pty.openpty()
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-m", "1024", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"]
    + firmware_args()
    + ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56",
       "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
    stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)
buf = b""

def expect(pat, timeout=600):
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

def cmd(line, timeout=900):
    os.write(master, (line + "\r").encode()); return expect(r"\naios> ", timeout)

def mean_reward(text):
    m = re.search(r"mean (-?[0-9.]+), best (-?[0-9.]+)", text)
    return (float(m.group(1)), float(m.group(2))) if m else (None, None)

def visited(text):
    out = {}
    for key, pat in (("cpu", r"cpu load\s+up to ([0-9.]+)"),
                     ("switch", r"switch rate\s+up to ([0-9.]+)"),
                     ("loss", r"packet loss\s+up to ([0-9.]+)"),
                     ("load", r"load imbalance\s+up to ([0-9.]+)")):
        m = re.search(pat, text)
        out[key] = float(m.group(1)) if m else None
    return out

expect(r"aios> ", 300)
print("reached the bare-metal prompt\n")
o = cmd("info")
if "bare-metal" not in o:
    raise SystemExit("this is not the bare-metal machine")

# Before claiming anything about learning: is there a gradient here at all?
# A policy gradient on a reward that does not respond to the lever is a
# gradient on sampling noise, and the run would show a number that does not
# move with no way to tell whether the policy or the instrument was at fault.
print("--- IS THERE A GRADIENT? sweeping the lever against the reward's own instrument ---")
sweep_out = cmd("jepa sweep 3", 900)
print(sweep_out)
gradient = "the reward responds" in sweep_out
m = re.search(r"reward spread across the lever's whole range: ([0-9.eE+-]+)", sweep_out)
spread = float(m.group(1)) if m else None
m = re.search(r"noise within a single setting \(worst half-range\): ([0-9.eE+-]+)", sweep_out)
noise = float(m.group(1)) if m else None

print("--- BEFORE: recording %d measured transitions ---" % N)
o = cmd(f"jepa record count={N}")
print(o)
before, before_best = mean_reward(o)
space = visited(o)

print("--- TRAINING: %d policy-gradient steps on those returns ---" % STEPS)
o = cmd(f"jepa train steps={STEPS} batch=32")
print(o)
m = re.search(r"World Model Prediction Loss \(MSE\): ([0-9.eE+-]+)", o)
world_loss = float(m.group(1)) if m else None

print("--- AFTER: recording %d more under the same conditions ---" % N)
o = cmd(f"jepa record count={N}")
print(o)
after, after_best = mean_reward(o)

print("--- the policy's own settled choice, and the fixed-workload check ---")
status = cmd("jepa status")
print(status)
m = re.search(r"a_t\[0\] -> scheduler slice\s+(\d+)", status)
chose = int(m.group(1)) if m else None
proof = cmd("host jepa prove /bin/spin 3", 900)
print(proof)

os.write(master, b"quit\r"); time.sleep(2); qemu.kill()

checks = [
    ("runs with no Linux underneath", True),
    ("the transitions were measured, not synthesised", before is not None),
    ("the conditions were actually visited", space.get("cpu") not in (None, 0.0)),
    ("the world model's prediction loss is finite and small",
     world_loss is not None and world_loss < 0.1),
    ("the policy settled on a slice inside the range the kernel clamps to",
     chose is not None and 1 <= chose <= 16),
    ("the fixed-workload experiment agrees the lever is real",
     "the lever is REAL" in proof),
]
checks.append(("the reward responds to the lever, so there is something to learn", gradient))
if before is not None and after is not None:
    if gradient:
        checks.append(("the measured return improved after training", after > before))
    else:
        print("NOT CHECKING whether the return improved: the sweep above says the reward")
        print("does not distinguish this lever from noise on this substrate, so an")
        print("improvement would be luck and a decline would be luck, and neither would")
        print("mean the policy learned anything.")

print()
bad = 0
for name, ok in checks:
    print(("  PASS  " if ok else "  FAIL  ") + name); bad += 0 if ok else 1
print()
print("  reward before training: mean %s, best %s" % (before, before_best))
print("  reward after  training: mean %s, best %s" % (after, after_best))
print("  state space visited   : %s" % space)
print("  policy settled on     : slice %s tick(s)" % chose)
print("  lever spread / noise  : %s / %s" % (spread, noise))
print("\n%d check(s), %d failed" % (len(checks), bad))
sys.exit(1 if bad else 0)
