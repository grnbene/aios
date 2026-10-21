#!/usr/bin/env python3
"""Drive the kernel's SSH client and git transport ON BARE METAL.

No host operating system is involved on the machine being driven: the ISO boots
under QEMU with a virtio-net card and SLIRP behind it, AOS's own TCP/IP stack
takes a DHCP lease, and then the kernel opens an SSH-2 session to the sshd on
the host at 10.0.2.2 — its own curve25519 key exchange, its own ed25519 host key
check, its own chacha20-poly1305 packet layer — and clones a repository over the
git wire protocol it also implements itself.

The machine generates its own identity on boot, so this drive reads the public
key off the serial console and authorises it for the duration of the run,
removing exactly that line afterwards. Point it somewhere else with:
    AIOS_METAL_SSH_USER=<account on this host>   (default: $USER)
Usage: drive-remote.py [ISO] [git-daemon-port]
"""
import os, sys, pty, time, re, subprocess, select, getpass, shutil, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

HERE = os.path.dirname(os.path.abspath(__file__))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "metal", "aios-metal.iso")
USER = os.environ.get("AIOS_METAL_SSH_USER", getpass.getuser())
AUTH = os.path.expanduser("~/.ssh/authorized_keys")
# The machine makes a new identity every boot, so the ssh legs can only run if
# this drive may authorise that identity on this host for the length of the run.
# Off by default: a test suite should not touch an account's authorized_keys
# because it happened to be invoked.
MAY_AUTHORISE = os.environ.get("AIOS_METAL_AUTHORIZE", "") not in ("", "0", "no")

ok = True
def check(cond, what):
    global ok
    ok = ok and bool(cond)
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)

# --------------------------------------------------------------- the upstream
work = tempfile.mkdtemp(prefix="aios-metal-remote-")
src, bare = os.path.join(work, "upstream"), os.path.join(work, "upstream.git")
def git(*a, cwd=None):
    return subprocess.run(["git"] + list(a), cwd=cwd, check=True, capture_output=True, text=True).stdout
git("init", "-q", src)
open(os.path.join(src, "README.md"), "w").write("cloned by a kernel with no operating system under it\n")
open(os.path.join(src, "history.txt"), "w").write("generation 1\n")
git("add", "-A", cwd=src)
git("-c", "user.email=drive@aios", "-c", "user.name=drive", "commit", "-qm", "a repository for the metal to fetch", cwd=src)
git("clone", "-q", "--bare", src, bare)
head = git("-C", bare, "rev-parse", "HEAD").strip()

# A port nothing else holds: a stale daemon on a fixed port would serve a
# directory that no longer exists and the drive would blame the kernel for it.
import socket, signal
if len(sys.argv) > 2:
    port = int(sys.argv[2])
else:
    s0 = socket.socket()
    s0.bind(("", 0))
    port = s0.getsockname()[1]
    s0.close()
daemon = subprocess.Popen(["git", "daemon", "--base-path=" + work, "--export-all", "--enable=receive-pack",
                           "--listen=0.0.0.0", "--port=%d" % port, "--reuseaddr"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          start_new_session=True)
time.sleep(1)

# --------------------------------------------------------------------- the VM
mpty, spty = pty.openpty()
cmd = ["qemu-system-x86_64", "-m", "1024", "-smp", "2", "-nographic", "-no-reboot",
       "-cdrom", ISO, "-boot", "d"] + firmware_args() + \
      ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56"]
q = subprocess.Popen(cmd, stdin=spty, stdout=spty, stderr=subprocess.DEVNULL, close_fds=True)
os.close(spty)

buf = ""
def pump(t=1.0):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([mpty], [], [], 0.2)
        if mpty in r:
            try: d = os.read(mpty, 65536)
            except OSError: break
            if not d: break
            buf += d.decode("utf-8", "replace")

def wait(pat, t=180):
    end = time.time() + t
    while time.time() < end:
        if re.search(pat, buf): return True
        pump(0.5)
    return False

def run(line, t=60):
    global buf
    buf = ""
    os.write(mpty, (line + "\r").encode())
    wait(r"\naios> ", t)
    return re.sub(r"\x1b\[[0-9;=?]*[a-zA-Z]", "", buf)

authorised_line = None
try:
    assert wait(r"aios> ", 300), "the machine never reached its prompt"
    up = run("host net up", 40)
    print(up.strip(), flush=True)
    check("up: ip 10.0.2.15" in up, "AOS's own TCP/IP took a DHCP lease — no host stack anywhere")

    keygen = run("ssh keygen", 40)
    print(keygen.strip(), flush=True)
    m = re.search(r"(ssh-ed25519 [A-Za-z0-9+/=]+ \S+)", keygen)
    check(m is not None, "the machine generated an ed25519 identity of its own")
    if not m:
        raise SystemExit(1)
    if MAY_AUTHORISE:
        authorised_line = m.group(1)
        with open(AUTH, "a") as f:
            f.write(authorised_line + "\n")
        print("  --    authorised %s… on this host for the run" % authorised_line[:44], flush=True)
    else:
        print("  --    ssh legs skipped: set AIOS_METAL_AUTHORIZE=1 to let this drive authorise", flush=True)
        print("  --    the machine's freshly generated key on %s for the length of the run" % AUTH, flush=True)

    if MAY_AUTHORISE:
        out = run("ssh %s@10.0.2.2 uname -sm" % USER, 60)
        print(out.strip(), flush=True)
        check("curve25519-sha256 + chacha20-poly1305" in out, "curve25519 key exchange, chacha20-poly1305 transport")
        check(re.search(r"host key\s*: SHA256:", out) is not None, "the host key was verified and recorded")
        check("Linux" in out and "exit          : 0" in out, "a command ran on the host and its output came back")

    out = run("git ls-remote git://10.0.2.2:%d/upstream.git" % port, 60)
    print(out.strip(), flush=True)
    check(head in out, "the remote's refs came back over the git wire protocol")

    out = run("git clone git://10.0.2.2:%d/upstream.git metal-clone" % port, 120)
    print(out.strip(), flush=True)
    check(head in out, "the pack was indexed and the branch landed at the remote's HEAD")
    cat = run("git use metal-clone", 30) + run("git log max=1", 30)
    check(head in cat, "the cloned history reads back out of the machine's own object database")
    files = run("git status", 30)
    print(files.strip(), flush=True)
    check("work tree clean" in files, "the work tree it checked out matches the commit it fetched")
    # The compatibility shell has its own namespace rooted at the kernel root;
    # the machine's real paths are under /host there.
    catted = run("cat /host/aios/root/repos/metal-clone/README.md", 20)
    print(catted.strip(), flush=True)
    check("no operating system under it" in catted, "the checked-out file reads back through the shell")

    # The other direction: a commit made on the metal, packed there, pushed here.
    run("echo the metal wrote this > /host/aios/root/repos/metal-clone/FROM-METAL.txt", 20)
    out = run("git add FROM-METAL.txt", 30) + run("git commit a commit made on the bare metal", 30)
    print(out.strip(), flush=True)
    out = run("git push git://10.0.2.2:%d/upstream.git main" % port, 120)
    print(out.strip(), flush=True)
    check("unpack ok" in out, "the remote unpacked the pack the metal kernel built")
    pushed = git("-C", bare, "rev-parse", "HEAD").strip()
    # The push report names the ref move in short ids, the way git does.
    check(pushed[:12] in out, "the remote's branch moved to the commit made on the metal (%s)" % pushed[:12])
    body = git("-C", bare, "show", "HEAD:FROM-METAL.txt")
    check("the metal wrote this" in body, "real git reads the file back out of what the metal pushed")

    if MAY_AUTHORISE:
        # The remote has moved on since the first clone: it now holds what this
        # machine pushed, and a fresh clone must come back with exactly that.
        out = run("git clone ssh://%s@10.0.2.2%s over-ssh" % (USER, bare), 180)
        print(out.strip(), flush=True)
        check(pushed in out, "the same clone again over ssh, and it brings back what the metal pushed")

    print("METAL REMOTE: " + ("PASS" if ok else "FAIL"), flush=True)
    sys.exit(0 if ok else 1)
finally:
    q.kill()
    try: os.killpg(os.getpgid(daemon.pid), signal.SIGTERM)
    except Exception: daemon.kill()
    if authorised_line:
        lines = [l for l in open(AUTH).read().splitlines() if l.strip() != authorised_line]
        open(AUTH, "w").write("\n".join(lines) + "\n")
    shutil.rmtree(work, ignore_errors=True)
