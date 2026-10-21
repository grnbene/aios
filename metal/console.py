#!/usr/bin/env python3
"""Attach to a live AOS virtual machine's console.

Starts AOS in QEMU with its serial console owned by a small proxy, so a person
(or a mind) can send a line and read what came back, one exchange at a time,
from separate invocations. This is an operator instrument, like a serial cable
into the back of the machine — nothing here is part of AOS.

  console.py start --disk IMG [--iso ISO] [--smp N] [--mem M] [--mind SOCK]
  console.py send "info"      send a line, wait for quiet, print what came back
  console.py read             print anything new since the last read
  console.py stop
"""
import os, pty, re, select, signal, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

DIR = os.environ.get("AIOS_CONSOLE_DIR", "/tmp/aios-console")
LOG, FIFO, PIDF, CURSOR = (os.path.join(DIR, n) for n in ("log", "in", "qemu.pid", "cursor"))


def _proxy(cmd):
    """Own the VM's console: pump QEMU output into the log and the fifo into QEMU."""
    master, slave = pty.openpty()
    p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    open(PIDF, "w").write("%d %d\n" % (p.pid, os.getpid()))
    fifo = os.open(FIFO, os.O_RDONLY | os.O_NONBLOCK)
    log = open(LOG, "ab", buffering=0)
    while p.poll() is None:
        r, _, _ = select.select([master, fifo], [], [], 0.5)
        if master in r:
            try:
                data = os.read(master, 65536)
            except OSError:
                break
            if data:
                log.write(data)
        if fifo in r:
            try:
                line = os.read(fifo, 4096)
            except OSError:
                line = b""
            if line:
                os.write(master, line)
            else:                      # every writer closed; reopen for the next one
                os.close(fifo)
                fifo = os.open(FIFO, os.O_RDONLY | os.O_NONBLOCK)
    log.close()


def start(args):
    disk = iso = None
    smp, mem = "4", "1024"
    mind = None   # --mind SOCK: expose the machine's second serial port (COM2) as a unix socket for a wire mind (metal/mind-wire.py)
    for i, a in enumerate(args):
        if a == "--disk": disk = os.path.abspath(args[i + 1])
        elif a == "--iso": iso = os.path.abspath(args[i + 1])
        elif a == "--smp": smp = args[i + 1]
        elif a == "--mind": mind = os.path.abspath(args[i + 1])
        elif a == "--mem": mem = args[i + 1]
    stop([])
    os.makedirs(DIR, exist_ok=True)
    for f in (LOG, CURSOR):
        if os.path.exists(f): os.remove(f)
    open(LOG, "wb").close(); open(CURSOR, "w").write("0")
    if os.path.exists(FIFO): os.remove(FIFO)
    os.mkfifo(FIFO)
    cmd = ["qemu-system-x86_64", "-smp", smp, "-m", mem, "-nographic", "-no-reboot"]
    if mind: cmd += ["-serial", "mon:stdio", "-serial", "unix:%s,server,nowait" % mind]   # COM1 stays the console on stdio; COM2 is the wire
    cmd += firmware_args(force_uefi=iso is None)   # a disk image built without BIOS grub needs OVMF
    if disk: cmd += ["-drive", "file=%s,if=virtio,format=raw" % disk, "-boot", "c"]
    if iso: cmd += ["-cdrom", iso, "-boot", "d"]
    if os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.R_OK | os.W_OK):
        cmd += ["-enable-kvm", "-cpu", "host"]
    if os.fork() == 0:                              # detach the proxy from this invocation
        os.setsid()
        # Release the caller's stdio, or whoever invoked us waits forever for EOF.
        null = os.open(os.devnull, os.O_RDWR)
        for fd in (0, 1, 2):
            os.dup2(null, fd)
        _proxy(cmd)
        os._exit(0)
    time.sleep(1)
    print("started: " + " ".join(cmd))


def _wait_quiet(timeout=90, quiet=1.2):
    """Wait until the console stops producing output (or we run out of patience)."""
    t0 = time.time(); last_size = -1; last_change = time.time()
    while time.time() - t0 < timeout:
        size = os.path.getsize(LOG)
        if size != last_size:
            last_size = size; last_change = time.time()
        elif time.time() - last_change >= quiet:
            return
        time.sleep(0.2)


def _drain():
    cur = int(open(CURSOR).read() or 0) if os.path.exists(CURSOR) else 0
    with open(LOG, "rb") as f:
        f.seek(cur)
        data = f.read()
        open(CURSOR, "w").write(str(f.tell()))
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)      # strip ANSI
    text = text.replace("\r\n", "\n").replace("\r", "")
    return text


def send(args):
    line = args[0] if args else ""
    with open(FIFO, "w") as f:
        f.write(line + "\r")
    time.sleep(0.4)
    _wait_quiet()
    sys.stdout.write(_drain())


def read(args):
    _wait_quiet(timeout=int(args[0]) if args else 20)
    sys.stdout.write(_drain())


def stop(args):
    if os.path.exists(PIDF):
        try:
            qemu_pid, proxy_pid = (int(x) for x in open(PIDF).read().split())
            for pid in (qemu_pid, proxy_pid):
                try: os.kill(pid, signal.SIGKILL)
                except ProcessLookupError: pass
        except Exception:
            pass
        os.remove(PIDF)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    {"start": start, "send": send, "read": read, "stop": stop}[sys.argv[1]](sys.argv[2:])
