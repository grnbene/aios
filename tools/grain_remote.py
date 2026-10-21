#!/usr/bin/env python3
"""grain_remote.py - code on the bare-metal Pi from here.

Everything goes over the Pi's UDP console (port 42424). Code runs in a
persistent session on the board (`grain repl`), so what one command defines
the next can use; files live on the Pi's SD card (8.3 names).

  grain_remote.py eval "let x = 21"          run in the session
  grain_remote.py eval "print(x * 2)"        ...which remembers x
  grain_remote.py run  prog.gr               upload + run in a fresh VM
  grain_remote.py load lib.gr                upload + run INTO the session
  grain_remote.py repl                       interactive session
  grain_remote.py push prog.gr [NAME.GR]     upload a file
  grain_remote.py pull NAME.GR [dest]        download a file (exact bytes)
  grain_remote.py ls | rm NAME | reset       card listing / delete / new session
  grain_remote.py shell "cpu"                any console command
  grain_remote.py discover                   find the board on the LAN
  grain_remote.py shot [out.png]             the board's screen, as a PNG (tcp/42427)
  grain_remote.py log                        follow the board's live breadcrumbs (udp/42425)

The board: --pi IP, else $GRAIN_PI, else its beacon (udp/42423, ~5 s
interval), else $AIOS_PI. The original flags still work: bare CODE (eval),
-f FILE (run), -i (repl), --discover.

  --fleet    route every console operation (eval/run/load/push/pull/ls/rm/
             reset/shell/repl) through the fleet's node link (authenticated,
             end-to-end encrypted through chi's hub) instead of the open UDP
             console - works with the console locked (`console debug off`,
             the default each boot). shot/log are unaffected either way
             (separate ports, not gated by the console lock at all).
"""
import argparse
import base64
import os
import re
import socket
import sys
import time

BEACON_PORT = 42423
CONSOLE_PORT = 42424
DEFAULT_PI = os.environ.get("AIOS_PI", "")   # the board's IP: --pi or AIOS_PI
MAX_COMMAND = 1400            # the console refuses longer commands
UPLOAD_CHUNK = 900            # base64 characters per `fs b64` command
CHUNK_HDR = re.compile(r"\[(\d+)/(\d+)\] ?")

# --fleet routes `shell`/`eval` through the fleet's node link (authenticated,
# end-to-end encrypted, chi's hub relays only ciphertext) instead of the raw,
# unauthenticated UDP console anyone on the LAN can already reach. It shells
# out to the host CLI rather than reimplementing the node protocol's crypto
# handshake here - that implementation is already verified, this would only
# duplicate it with a second copy to keep in sync.
FLEET_HUB_HOST = os.environ.get("AIOS_FLEET_HUB", "")
FLEET_HUB_PORT = "7420"
FLEET_KEY_FILE = "/etc/aos/fleet.key"
FLEET_PEER_PREFIX = "ce4d"    # the Pi's fleet identity (ce4db529c75c...)


# ---- console transport ------------------------------------------------------

def console(pi, cmd, quiet=0.6, total=30.0, resend=2.5):
    """Send one console command; return its whole reply, chunks in order.

    At-most-once: the command goes as "@ID command". If no reply shows up, or
    a multi-chunk reply arrives with holes, the same ID is sent again - the
    board ignores it while the command is still running and answers a
    finished one from its kept reply, so a lost frame never runs the command
    twice. Nothing comes back until the command has finished, so the quiet
    window starts at the first packet."""
    rid = os.urandom(6).hex()
    wire = f"@{rid} {cmd}".encode()
    if len(wire) > MAX_COMMAND:
        raise ValueError(f"console command is {len(wire)} bytes; the board takes {MAX_COMMAND}")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.2)
    s.sendto(wire, (pi, CONSOLE_PORT))
    chunks, plain, last, want = {}, [], None, None
    start = sent = time.time()
    try:
        while time.time() - start < total:
            if want is not None and len(chunks) == want:
                break
            if last is not None and time.time() - last > quiet:
                if want is None or len(chunks) == want:
                    break
                if time.time() - sent > quiet:       # holes in a chunked reply: ask again
                    s.sendto(wire, (pi, CONSOLE_PORT))
                    sent = time.time()
            elif last is None and time.time() - sent > resend:
                s.sendto(wire, (pi, CONSOLE_PORT))   # lost, or still running - both are safe
                sent = time.time()
            try:
                data, _ = s.recvfrom(65535)
            except socket.timeout:
                continue
            last = time.time()
            text = data.decode(errors="replace")
            m = CHUNK_HDR.match(text)
            if m:
                chunks[int(m.group(1))] = text[m.end():]
                want = int(m.group(2))
            else:
                plain.append(text)
    finally:
        s.close()
    if last is None:
        raise TimeoutError(f"no reply from {pi}:{CONSOLE_PORT} in {total:.0f}s")
    if not chunks:
        return plain[0]                              # a single-frame reply (resends repeat it)
    reply = "".join(chunks[i] for i in sorted(chunks))
    if len(chunks) != want:
        missing = sorted(set(range(1, want + 1)) - set(chunks))
        reply += f"  [grain_remote: reply incomplete - chunks {missing} of {want} never arrived]\n"
    return reply


def fleet_console(cmd, peer=FLEET_PEER_PREFIX, timeout=60.0):
    """One command, through the fleet's node link instead of the raw UDP
    console: `aios exec` joins the hub and runs `fleet run <peer> <cmd>` in
    the same process (fleet membership is per-process state, not persisted
    between invocations - see cli/commands.cpp's cmd_fleet). Raises
    TimeoutError on the same conditions console() does, so callers do not
    need to know which transport actually ran."""
    if not FLEET_HUB_HOST:
        raise SystemExit("--fleet needs the hub address: set AIOS_FLEET_HUB (and AIOS_FLEET_HUB_PORT, default 7420)")
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    aios_bin = os.path.join(here, "..", "build", "aios")
    if not os.path.exists(aios_bin):
        raise SystemExit(f"--fleet needs the host CLI built: {aios_bin} not found (cmake --build build)")
    # `fleet run <peer> @FILE` (cli/commands.cpp), not the command
    # embedded inline: `aios exec` tokenizes each argument it is given by
    # whitespace/quotes before cmd_fleet ever sees it, and grain source
    # routinely carries ", {, }, ? and : - exactly what that tokenizer would
    # mangle. A temp file sidesteps any of that: the CLI reads it as raw
    # bytes, unparsed.
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".fleetcmd", delete=False) as f:
        f.write(cmd)
        tmp_path = f.name
    try:
        args = [aios_bin, "exec",
                f"fleet join {FLEET_HUB_HOST} {FLEET_HUB_PORT} @{FLEET_KEY_FILE}",
                "sleep 3",
                f"fleet run {peer} @{tmp_path}"]
        try:
            r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise TimeoutError(f"fleet run {peer} {cmd!r} did not answer in {timeout:.0f}s")
    finally:
        os.unlink(tmp_path)
    lines = r.stdout.splitlines()
    # cmd_fleet's own "joined the fleet via ..." confirmation is the only
    # thing `exec` prints before the command's own output (no `fleet
    # members` call here to add more) - drop exactly that one line.
    if lines and lines[0].startswith("joined the fleet"):
        lines = lines[1:]
    if r.returncode != 0 and "FAILED:" in r.stderr:
        raise SystemExit(r.stderr.strip())
    return "\n".join(lines)


def clean(reply):
    """The console indents everything by two spaces; drop that and the
    `[output saved ...]` note."""
    out = []
    for line in reply.splitlines():
        if line.startswith("  [output "):
            continue
        out.append(line[2:] if line.startswith("  ") else line)
    return "\n".join(out).rstrip("\n")


# ---- files on the card ---------------------------------------------------------

def card_name(path):
    stem = re.sub(r"[^A-Z0-9]", "", os.path.splitext(os.path.basename(path))[0].upper())[:8] or "PROG"
    return stem + ".GR"


def _run(pi, cmd, via_fleet, total):
    """One console command, either transport - the `fs put`/`fs get`/`grain
    run`/`grain load` vocabulary underneath is unchanged either way, only
    where it travels differs."""
    return fleet_console(cmd, timeout=total) if via_fleet else console(pi, cmd, total=total)


def push(pi, path, name=None, via_fleet=False):
    """Upload through `fs put`: chunks are staged in the board's RAM and the
    card is written once, at `end`. Every chunk goes at-most-once over the
    raw console (see console()); fleet run has no such retry (a TCP node
    link, not a lossy UDP datagram) and no 1400-byte datagram limit either -
    ROOM is ~800x bigger there, so a typical script is one chunk instead of
    dozens."""
    data = open(path, "rb").read()
    name = name or card_name(path)
    b64 = base64.b64encode(data).decode()
    if via_fleet:
        room = 700000  # base64 chars/command - well under a subprocess argv limit, far above MAX_COMMAND
    else:
        room = MAX_COMMAND - len(f"@{'0' * 12} fs put {name} new ") - 4
    room -= room % 4
    staged, first = 0, True
    for i in range(0, max(len(b64), 1), room):
        part = b64[i:i + room]
        reply = _run(pi, f"fs put {name} {'new ' if first else ''}{part}", via_fleet, 60 if via_fleet else 30)
        first = False
        m = re.search(r"staged (\d+) bytes", reply)
        staged = len(base64.b64decode(b64[:i + room]))
        if not m or int(m.group(1)) != staged:
            raise SystemExit(f"upload of {name} failed at byte {staged}: {clean(reply)}")
    reply = _run(pi, f"fs put {name} end", via_fleet, 120)
    m = re.search(r"ok, (\d+) bytes written", reply)
    if not m or int(m.group(1)) != len(data):
        raise SystemExit(f"upload of {name} failed writing the card: {clean(reply)}")
    return name


def pull(pi, name, via_fleet=False):
    out, off, size = b"", 0, None
    while size is None or off < size:
        reply = _run(pi, f"fs get {name} {off}", via_fleet, 10 if not via_fleet else 20)
        m = re.search(r"size=(\d+) off=(\d+) len=(\d+) b64=(\S*)", reply)
        if not m:
            raise SystemExit(clean(reply) or f"pull {name}: no answer")
        size, length = int(m.group(1)), int(m.group(3))
        piece = base64.b64decode(m.group(4))
        if len(piece) != length:
            raise SystemExit(f"pull {name}: slice at {off} arrived damaged ({len(piece)} of {length} bytes)")
        out += piece
        off += length
        if length == 0:
            break
    return out


# ---- running code ---------------------------------------------------------------

def eval_code(pi, code, timeout=60.0, via_fleet=False):
    """In the session. Short code goes as one console command; longer code is
    uploaded and loaded (same session, same effect)."""
    cmd = f"grain repl {code}"
    fits = len(cmd.encode()) <= MAX_COMMAND
    if via_fleet and fits:
        return clean(fleet_console(cmd, timeout=timeout))
    if fits:
        return clean(console(pi, cmd, total=timeout))
    tmp = "/tmp/grain_remote_session.gr"
    with open(tmp, "w") as f:
        f.write(code)
    push(pi, tmp, "SESSION.GR", via_fleet=via_fleet)
    return clean(_run(pi, "grain load SESSION.GR", via_fleet, timeout))


def run_file(pi, path, timeout=300.0, via_fleet=False):
    name = push(pi, path, via_fleet=via_fleet)
    out = os.path.splitext(name)[0] + ".OUT"
    return clean(_run(pi, f"grain run {name} {out}", via_fleet, timeout))


def load_file(pi, path, timeout=300.0, via_fleet=False):
    name = push(pi, path, via_fleet=via_fleet)
    return clean(_run(pi, f"grain load {name}", via_fleet, timeout))


def listing(pi, via_fleet=False):
    return eval_code(pi, 'for e in list_dir() { print(e.dir ? "<dir>" : str(e.size), e.name) }', via_fleet=via_fleet)


# ---- discovery ---------------------------------------------------------------------

def local_ipv4():
    """This machine's address on the default route (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 9))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def arp_neighbours():
    """IPv4 neighbours the kernel already has a MAC for (Linux), so the
    likeliest answers are asked first."""
    try:
        with open("/proc/net/arp") as f:
            rows = f.read().splitlines()[1:]
    except OSError:
        return []
    return [r.split()[0] for r in rows if len(r.split()) > 3 and r.split()[3] != "00:00:00:00:00:00"]


def discover(timeout=6.0):
    """Find boards on the LAN. First by asking: the console query below goes
    to the broadcast address and to every address of this /24 - answers to
    the unicast ones are replies to our own datagrams, which a host firewall
    that drops unsolicited traffic (ufw's default) lets in, where it drops the
    answer to a broadcast. Then by listening for the beacon the kernel
    broadcasts on udp/42423 every ~5 s (such a firewall drops that too)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    probe = f"@{os.urandom(6).hex()} grain repl print(sys.arch, sys.host)".encode()
    found = {}
    try:
        s.setblocking(False)                  # an address nobody answers ARP for must not stall the sweep
        targets = ["255.255.255.255"]
        me = local_ipv4()
        if me:
            net = me.rsplit(".", 1)[0]
            first = [ip for ip in arp_neighbours() if ip.startswith(net + ".")]
            targets += first + [f"{net}.{h}" for h in range(1, 255) if f"{net}.{h}" not in first and f"{net}.{h}" != me]
        for ip in targets:
            try:
                s.sendto(probe, (ip, CONSOLE_PORT))
            except OSError:
                pass                             # nobody there, or the queue is full: either way, no board
        s.setblocking(True)
        s.settimeout(0.3)
        start = time.time()
        while time.time() - start < min(timeout, 1.5):
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            text = CHUNK_HDR.sub("", data.decode(errors="replace")).strip()
            found.setdefault(addr[0], {"ip": addr[0], "mac": "?", "raw": text})
    finally:
        s.close()
    if found:
        return sorted(found.values(), key=lambda p: p["ip"])[0]
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    b.settimeout(0.5)
    try:
        b.bind(("", BEACON_PORT))
    except OSError as e:
        print(f"[discover] cannot listen on udp/{BEACON_PORT}: {e}", file=sys.stderr)
        return None
    start = time.time()
    try:
        while time.time() - start < timeout:
            try:
                data, addr = b.recvfrom(1024)
            except socket.timeout:
                continue
            msg = data.decode(errors="replace").strip()
            if msg.startswith("AIOS-METAL-ARM64"):
                parts = msg.split()
                return {"ip": parts[1] if len(parts) > 1 else addr[0], "mac": parts[2] if len(parts) > 2 else "?", "raw": msg}
    finally:
        b.close()
    return None


def board_answers(pi):
    try:
        console(pi, "grain repl", total=1.5)
        return True
    except TimeoutError:
        return False


def pick_board(explicit):
    if explicit:
        return explicit
    if os.environ.get("GRAIN_PI"):
        return os.environ["GRAIN_PI"]
    if board_answers(DEFAULT_PI):
        return DEFAULT_PI
    peer = discover(timeout=6.0)
    if peer:
        print(f"[grain_remote] found the board at {peer['ip']} by its beacon", file=sys.stderr)
        return peer["ip"]
    return DEFAULT_PI


# ---- interactive --------------------------------------------------------------------

def depth(text):
    """Unclosed { ( [ in text, ignoring strings and # comments - how the REPL
    knows a block is still open."""
    d, quote, i = 0, None, 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "#":
            while i < len(text) and text[i] != "\n":
                i += 1
        elif c in "{([":
            d += 1
        elif c in "})]":
            d -= 1
        i += 1
    return d


def repl(pi, via_fleet=False):
    print(f"grain on {pi} - a persistent session{' (via fleet)' if via_fleet else ''}. "
          f":reset  :load FILE  :ls  !console-command  :quit")
    buf = ""
    while True:
        try:
            line = input("....  " if buf else "grain> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not buf:
            cmd = line.strip()
            if cmd in (":quit", ":q", "exit", "quit"):
                return
            if not cmd:
                continue
            if cmd == ":reset":
                print(clean(_run(pi, "grain reset", via_fleet, 30)))
                continue
            if cmd == ":ls":
                print(listing(pi, via_fleet=via_fleet))
                continue
            if cmd.startswith(":load "):
                print(load_file(pi, cmd[6:].strip(), via_fleet=via_fleet))
                continue
            if cmd.startswith("!"):
                print(clean(_run(pi, cmd[1:].strip(), via_fleet, 60)))
                continue
        buf += line + "\n"
        if depth(buf) > 0:
            continue
        code, buf = buf.rstrip("\n"), ""
        try:
            out = eval_code(pi, code, via_fleet=via_fleet)
        except TimeoutError as e:
            out = f"[{e}]"
        if out:
            print(out)


# ---- looking at the board --------------------------------------------------------

def shot(pi, out):
    """The framebuffer, full resolution: the board's screenshot server
    (tcp/42427) sends one QOI image per connection; metal/qoi2png.py
    decodes it."""
    import subprocess
    s = socket.create_connection((pi, 42427), timeout=10)
    s.settimeout(20)
    data = b""
    try:
        while True:
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            data += chunk
    finally:
        s.close()
    if not data.startswith(b"qoif"):
        raise SystemExit(f"shot: the board sent {len(data)} bytes that are not a QOI image")
    qoi = out[:-4] + ".qoi" if out.endswith(".png") else out + ".qoi"
    with open(qoi, "wb") as f:
        f.write(data)
    here = os.path.dirname(os.path.abspath(__file__))
    subprocess.run([sys.executable, os.path.join(here, "..", "metal", "qoi2png.py"), qoi, out], check=True)
    os.unlink(qoi)
    w = int.from_bytes(data[4:8], "big")
    h = int.from_bytes(data[8:12], "big")
    return f"{out}: {w}x{h}"


def follow_log():
    """The kernel broadcasts one line per event on udp/42425 (boot steps, a
    heartbeat each second, panics) - the last lines are the last thing it did."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 42425))
    try:
        while True:
            data, addr = s.recvfrom(4096)
            print(time.strftime("%H:%M:%S"), addr[0], data.decode(errors="replace").rstrip(), flush=True)
    except (KeyboardInterrupt, BrokenPipeError):
        pass


# ---- entry ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Code on the bare-metal Pi over its UDP console.")
    ap.add_argument("--pi", "-t", "--target", dest="pi", default=None, help="the board's IP")
    ap.add_argument("--fleet", action="store_true",
                     help="route every console operation through the fleet's node link "
                          "(authenticated, encrypted) instead of the open UDP console - works with "
                          "the console locked. shot/log are unaffected either way (separate ports)")
    ap.add_argument("-f", "--file", help="(legacy) run this file")
    ap.add_argument("-i", "--interactive", action="store_true", help="(legacy) interactive session")
    ap.add_argument("-d", "--discover", action="store_true", help="(legacy) listen for the beacon")
    ap.add_argument("words", nargs="*", help="command and arguments (see the module docstring)")
    a = ap.parse_args()

    commands = {"eval", "run", "load", "repl", "push", "pull", "ls", "rm", "reset", "shell", "discover", "shot", "log"}
    cmd, rest = (a.words[0], a.words[1:]) if a.words and a.words[0] in commands else (None, a.words)
    if a.discover or cmd == "discover":
        peer = discover()
        print(f"board {peer['ip']}  ({peer['raw']})" if peer else f"no board answered or beaconed; it is usually {DEFAULT_PI}")
        return 0 if peer else 1

    if cmd == "log":
        follow_log()
        return 0
    if a.fleet and cmd == "shot":
        raise SystemExit("--fleet is meaningless for `shot` - it never touches the console "
                          "(tcp/42427), and works whether the console is locked or not; drop --fleet")
    pi = pick_board(a.pi)
    try:
        if cmd == "shot":
            print(shot(pi, rest[0] if rest else "pi-screen.png"))
            return 0
        if a.file:
            print(run_file(pi, a.file, via_fleet=a.fleet))
        elif a.interactive or cmd == "repl":
            repl(pi, via_fleet=a.fleet)
        elif cmd == "eval" or (cmd is None and rest):
            print(eval_code(pi, " ".join(rest), via_fleet=a.fleet))
        elif cmd == "run":
            print(run_file(pi, rest[0], via_fleet=a.fleet))
        elif cmd == "load":
            print(load_file(pi, rest[0], via_fleet=a.fleet))
        elif cmd == "push":
            print(push(pi, rest[0], rest[1] if len(rest) > 1 else None, via_fleet=a.fleet))
        elif cmd == "pull":
            data = pull(pi, rest[0], via_fleet=a.fleet)
            if len(rest) > 1:
                open(rest[1], "wb").write(data)
                print(f"{rest[0]}: {len(data)} bytes -> {rest[1]}")
            else:
                sys.stdout.buffer.write(data)
        elif cmd == "ls":
            print(listing(pi, via_fleet=a.fleet))
        elif cmd == "rm":
            print(clean(_run(pi, f"fs rm {rest[0]}", a.fleet, 30)))
        elif cmd == "reset":
            print(clean(_run(pi, "grain reset", a.fleet, 30)))
        elif cmd == "shell":
            print(clean(_run(pi, " ".join(rest), a.fleet, 60)))
        else:
            print(eval_code(pi, "print('grain on', sys.arch, '-', sys.cpus, 'cores,', int(sys.uptime_ms / 1000), 's up')", via_fleet=a.fleet))
    except TimeoutError as e:
        print(f"grain_remote: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
