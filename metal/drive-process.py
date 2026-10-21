#!/usr/bin/env python3
"""Drive the bare-metal process model: interrupts, ring 3, address spaces.

Boots the real ISO in QEMU, reaches the machine's own prompt over the serial
line, and then makes the machine prove — not report — that:

  * the timer interrupt exists (the CPU is halted, and something restarts it);
  * an ELF file on this machine's disk becomes a process in ring 3;
  * a process that touches address zero dies and the kernel does not;
  * a process that executes cli takes a general protection fault, which is what
    ring 3 means;
  * the kernel's own memory, mapped into every address space so interrupts can
    land, is unreadable from user space;
  * two processes that never make a system call still take turns, which can
    only happen if the timer takes the CPU away from them;
  * the same binary under two entities gets two different answers from the same
    system call, because authority is checked in ring 0 and not by the program.

  metal/drive-process.py [iso]
"""
import os, pty, re, select, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")

master, slave = pty.openpty()
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cpu", "max", "-m", "1024", "-nographic", "-no-reboot", "-cdrom", ISO, "-boot", "d"]
    + firmware_args() + ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
    stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)
buf = b""

def expect(pattern, timeout=180):
    global buf
    rx = re.compile(pattern.encode())
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = rx.search(buf)
        if m:
            out, buf = buf[:m.end()], buf[m.end():]
            return out.decode(errors="replace")
        r, _, _ = select.select([master], [], [], 1)
        if r:
            try:
                d = os.read(master, 65536)
            except OSError:
                d = b""
            if not d and qemu.poll() is not None:
                break
            buf += d
    raise SystemExit("TIMEOUT waiting for %r; tail:\n%s" % (pattern, buf[-3000:].decode(errors="replace")))

def cmd(line, timeout=180):
    os.write(master, (line + "\r").encode())
    return re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", expect(r"\naios> ", timeout))

checks = []
def check(name, ok, detail=""):
    checks.append((name, bool(ok)))
    print(("  ok   " if ok else "  FAIL ") + name + (("  |  " + detail) if detail else ""))

boot = expect(r"aios> ", 240)
boot = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", boot)
print("=== what the machine says about itself as it comes up")
for line in boot.splitlines():
    if line.startswith("[metal]"):
        print("  | " + line)
check("the IDT is installed before anything runs", "IDT live" in boot)
check("ring 3 and a user address space exist at boot", "processes: ring 3 ready" in boot)

print("\n=== the timer: the CPU is stopped, and something starts it again")
o = cmd("host irq")
print("  | " + "\n  | ".join(x for x in o.splitlines() if x and not x.startswith("host irq")))
m = re.search(r"ticks: (\d+) → (\d+) after halting", o)
check("halting the CPU 25 times still advances the clock", m and int(m.group(2)) > int(m.group(1)),
      m.group(0) if m else "no tick line")
check("the interrupt flag is actually set", "interrupt flag: set" in o)
check("and it says which controller is delivering them", "delivered by:" in o,
      next((x.strip() for x in o.splitlines() if x.startswith("delivered by")), ""))

print("\n=== an ELF file on this machine becomes a process in ring 3")
o = cmd("host run /bin/hello")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("it ran in user space and said so", re.search(r"hello from user space, pid \d+", o) is not None)
check("it read the kernel's clock through a system call", "ticks" in o)
check("it exited with its own code", re.search(r"/bin/hello: finished, exit 7", o) is not None)

print("\n=== a process that touches address zero")
o = cmd("host run /bin/crash")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the fault is reported with the address that caused it", "cr2=0x0000000000000000" in o)
check("the process is ended, not the machine", "finished, exit -14" in o)
o = cmd("info")
check("the kernel is still alive after it", "Kernel ID" in o)

print("\n=== a process that executes cli, an instruction only ring 0 may run")
o = cmd("host run /bin/priv")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("it takes a general protection fault", "general protection fault" in o)
check("it never got to turn interrupts off", "NOT ring 3" not in o)

print("\n=== the kernel's own memory, from user space")
o = cmd("host run /bin/peek")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("reading kernel memory faults at exactly that address", "cr2=0x0000000000100000" in o)
check("no byte of the kernel reached the program", "ring 3 is not enforced" not in o)

print("\n=== two processes that never make a system call while they work")
o = cmd("host run /bin/spin /bin/spin", 300)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
m = re.search(r"took the CPU back (\d+) time", o)
switches = int(m.group(1)) if m else 0
ticks = [int(x) for x in re.findall(r"/bin/spin: finished, exit \d+, (\d+) tick", o)]
check("both finished", len(ticks) == 2, "ticks accounted: %s" % ticks)
check("the scheduler took the CPU back many times", switches > 20, "%d context switches" % switches)
check("and both of them actually held it", len(ticks) == 2 and min(ticks) > 5,
      "neither ran to completion before the other started")
# Both were on the CPU at once: the later one started before the earlier one
# finished. Which of them finishes first is the scheduler's business and not
# something to assert.
starts = [int(t) for _, t in re.findall(r"spin (\d+) starting at tick (\d+)", o)]
ends = [int(t) for _, t in re.findall(r"spin (\d+) finished at tick (\d+)", o)]
check("they were running at the same time, not one after the other",
      len(starts) == 2 and len(ends) == 2 and max(starts) < min(ends),
      "started at ticks %s, finished at ticks %s" % (starts, ends))

print("\n=== the console is a task too: a program keeps running between prompts")
o = cmd("host spawn /bin/spin")
check("spawning comes straight back to the prompt", "comes straight back" in o)
first = cmd("host ps")
second = cmd("host ps")
print("  | " + "\n  | ".join(x for x in second.splitlines()[1:] if x.strip()))
def spin_ticks(text):
    m = re.search(r"/bin/spin\s+\w+\s+(\d+)", text)
    return int(m.group(1)) if m else -1
check("the console appears in its own task table", "console" in first and "running" in first)
check("the spawned program advanced between two prompts, with nothing else running",
      spin_ticks(second) > spin_ticks(first) >= 0,
      "ticks %s → %s while the console sat at the prompt" % (spin_ticks(first), spin_ticks(second)))
o = cmd("host wait", 300)
check("and waiting for it reports the switches it took", "the scheduler switched" in o)

print("\n=== file descriptors: this machine's own storage, opened by a process")
o = cmd("host run /bin/fs")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("a process wrote a file and read it back through descriptors", "survived the round trip" in o)
o = cmd("ls /tmp")
check("and the file is really on the machine", "written-by-a-process" in o)

print("\n=== memory a process asks for while it runs")
o = cmd("host run /bin/mem")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the break moved and the new heap holds what was written", "40 KiB of heap" in o)
check("an anonymous mapping arrived zeroed and usable", ", zeroed, and it holds what was written" in o)

print("\n=== fork: a child whose memory is its own")
o = cmd("host run /bin/forker")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the child sees itself as a child of its parent", re.search(r"child pid \d+, parent \d+", o) is not None)
check("the child's writes did not reach the parent", "parent's copy still says 11" in o)
check("and the parent collected its exit status", "it exited 42" in o)

print("\n=== exec: same process, a different program")
o = cmd("host run /bin/execer")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the child became another program and kept its pid",
      re.search(r"child (\d+) is about to become", o) and
      re.search(r"hello from user space, pid (\d+)", o) and
      re.search(r"child (\d+) is about", o).group(1) == re.search(r"hello from user space, pid (\d+)", o).group(1))
check("and the parent saw the new program's exit status", "exited 7" in o)

print("\n=== signals")
o = cmd("host spawn /bin/sig")
pid = re.search(r"as pid (\d+)", o).group(1)
o = cmd("host kill " + pid)
check("a signal can be sent from the console", "signal 15 sent" in o)
o = cmd("host wait", 300)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("the handler ran in the process, on its own stack", "handler ran on my own stack" in o)
check("and returning from it put the process back where it was", "put me back where I was" in o)
o = cmd("host spawn /bin/spin")
pid = re.search(r"as pid (\d+)", o).group(1)
cmd("host kill " + pid + " 9")
o = cmd("host wait", 300)
check("a signal with no handler ends the process, and says which signal", "killed by signal 9" in o)

print("\n=== floating point across a context switch")
o = cmd("host run /bin/fpu", 300)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
alone = re.search(r"computed (-?\d+)", o)
check("one instance computes a number", alone is not None, alone.group(0) if alone else "")
o = cmd("host run /bin/fpu /bin/fpu", 300)
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
both = re.findall(r"computed (-?\d+)", o)
check("two running at once get the SAME answer as one running alone",
      len(both) == 2 and both[0] == both[1] == alone.group(1),
      "alone %s, together %s" % (alone.group(1) if alone else "?", both))

print("\n=== the same binary, two entities, one system call")
o = cmd("host run /bin/probe")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("as the console it may write to this machine's storage", "bytes to this machine's storage" in o)
o = cmd("host run /bin/probe bare")
print("  | " + "\n  | ".join(x for x in o.splitlines()[1:] if x.strip()))
check("as a bare entity the identical binary is refused", "holds no write authority over storage" in o)
check("and the refusal is the process's exit status", "finished, exit 13" in o)
o = cmd("ls /tmp")
check("only the permitted run left a file behind", o.count("proc-") == 1, " ".join(o.split()[-3:]))

print("\n=== the machine after all of that")
o = cmd("host ps")
check("every finished process was reaped — only the console is left",
      "console" in o and "/bin/" not in o)
o = cmd("host heap")
check("the command surface still answers", "heap in use" in o)

os.write(master, b"quit\r")
time.sleep(2)
qemu.kill()
qemu.wait()

bad = [n for n, ok in checks if not ok]
print("\n%d/%d checks passed" % (len(checks) - len(bad), len(checks)))
print("PROCESS: " + ("PASS" if not bad else "FAIL (" + ", ".join(bad) + ")"))
sys.exit(0 if not bad else 1)
