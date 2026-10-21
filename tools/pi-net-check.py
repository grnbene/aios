#!/usr/bin/env python3
"""Drive grain's sockets and sys_exec on the Pi against real peers here.

Starts a UDP echo server, a TCP echo server and an unsolicited UDP sender on
this machine, then runs a grain program ON THE BOARD (through grain_remote)
that talks to them - and to itself - and checks every answer:

  UDP to itself (stays inside the kernel), UDP to this host and the echo
  back, a datagram this host sends first, a TCP HTTP request to the LAN
  router read to the end (tcp_recv -> "" when it closes), errors for a closed
  port / an oversized datagram / a kernel-reserved port, sys_exec running a
  console command.

  Also: a TCP round trip to a server on THIS machine and a UDP datagram the
  Pi starts unprimed - both need ufw to let the Pi in (`ufw allow in on br0
  from the board`, added 2026-09-25); without it they fail and the rest
  still pass.

  tools/pi-net-check.py [--pi <board ip>] [--host <this host ip>]
"""
import argparse
import os
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grain_remote  # noqa: E402

UDP_ECHO, PI_PORT, PI_ECHO_PORT, TCP_ECHO, UDP_SINK = 47001, 47101, 47102, 47002, 47003


def udp_echo(stop, pi):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", UDP_ECHO))
    s.settimeout(0.1)
    while not stop.is_set():
        s.sendto(b"prime", (pi, PI_ECHO_PORT))     # opens the firewall's return path
        try:
            data, addr = s.recvfrom(4096)
            s.sendto(b"echo:" + data, addr)
        except socket.timeout:
            pass
    s.close()


def tcp_echo(stop):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", TCP_ECHO))
    srv.listen(4)
    srv.settimeout(0.2)
    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        c.settimeout(5)
        try:
            c.sendall(b"echo:" + c.recv(4096))
            time.sleep(0.3)
        except OSError:
            pass
        c.close()
    srv.close()


def udp_sink(stop):
    """Answers whatever arrives - no priming, so only a firewall that lets
    the Pi in passes this one."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", UDP_SINK))
    s.settimeout(0.2)
    while not stop.is_set():
        try:
            data, addr = s.recvfrom(4096)
            s.sendto(b"sink:" + data, addr)
        except socket.timeout:
            pass
    s.close()


def unsolicited(stop, pi):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    while not stop.is_set():
        s.sendto(b"hello-from-host", (pi, PI_PORT))
        time.sleep(0.1)
    s.close()


PROGRAM = r'''
let host = "@HOST@"
fn show(label, v) { print(label + ": " + repr(v)) }

let me = udp_socket()
udp_bind(me, 47100)
udp_send(me, "127.0.0.1", 47100, "to-myself")
let got = udp_recv(me, 100, 1000)
show("udp self", [got.data, got.port])

let u = udp_socket()
udp_bind(u, 47102)
udp_send(u, host, 47001, "ping-udp")
let echo = nil
let tries = 0
while tries < 60 and (echo == nil or echo.data == "prime") { echo = udp_recv(u, 100, 100) tries += 1 }
show("udp echo", [echo.data, echo.host, echo.port])
udp_close(u)

let inbox = udp_socket()
udp_bind(inbox, 47101)
let first = udp_recv(inbox, 100, 3000)
show("udp from host", [first.data, first.host])
udp_close(inbox)

let t = tcp_connect("@ROUTER@", 80, 3000)
let req = "GET / HTTP/1.0\r\nHost: @ROUTER@\r\nConnection: close\r\n\r\n"
show("tcp send", tcp_send(t, req) == len(req))
let body = ""
let piece = tcp_recv(t, 4096, 3000)
let reads = 0
while piece != nil and piece != "" and reads < 200 { body += piece piece = tcp_recv(t, 4096, 3000) reads += 1 }
show("tcp http status", slice(body, 0, 7))
show("tcp read to close", piece)
show("tcp got a page", len(body) > 100)
tcp_close(t)

let ht = tcp_connect(host, 47002, 3000)
tcp_send(ht, "ping-tcp")
show("tcp to host", tcp_recv(ht, 100, 3000))
show("tcp host closed", tcp_recv(ht, 100, 3000))
tcp_close(ht)
let us = udp_socket()
udp_send(us, host, 47003, "unprimed")
let sr = udp_recv(us, 100, 3000)
show("udp unprimed", sr == nil ? nil : sr.data)
udp_close(us)

show("closed port", pcall(fn () { return tcp_connect(host, 1, 1000) }).ok)
show("too big", pcall(fn () { return udp_send(me, host, 47001, repeat("x", 2000)) }).error)
show("reserved", pcall(fn () { let r = udp_socket() return udp_bind(r, 42424) }).error)
udp_close(me)

let sx = sys_exec("cpu")
show("sys_exec", [sx.ok, contains(sx.output, "arm clock")])
show("sys_exec unknown", sys_exec("no-such-command").code)
'''

EXPECT = [
    ('udp self: ["to-myself", 47100]', None),
    ('udp echo: ["echo:ping-udp", "@HOST@", 47001]', None),
    ('udp from host: ["hello-from-host", "@HOST@"]', None),
    ("tcp send: true", None),
    ('tcp http status: "HTTP/1."', None),
    ('tcp read to close: ""', None),
    ("tcp got a page: true", None),
    ('tcp to host: "echo:ping-tcp"', None),
    ('tcp host closed: ""', None),
    ('udp unprimed: "sink:unprimed"', None),
    ("closed port: false", None),
    ('too big: "udp_send(): datagram larger than one frame (1472 bytes)"', None),
    ('reserved: "udp_bind(): port 42424 is in use (by the kernel or another socket)"', None),
    ("sys_exec: [true, true]", None),
    ("sys_exec unknown: 127", None),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pi", default=os.environ.get("GRAIN_PI", grain_remote.DEFAULT_PI))
    ap.add_argument("--host", default=None, help="this machine's address as the Pi sees it")
    ap.add_argument("--router", default=os.environ.get("AIOS_ROUTER", ""), help="a LAN host serving HTTP on port 80")
    a = ap.parse_args()
    host = a.host
    if not host:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((a.pi, 9))
        host = probe.getsockname()[0]
        probe.close()

    stop = threading.Event()
    threads = [threading.Thread(target=f, args=args, daemon=True)
               for f, args in ((udp_echo, (stop, a.pi)), (tcp_echo, (stop,)), (udp_sink, (stop,)), (unsolicited, (stop, a.pi)))]
    for t in threads:
        t.start()
    time.sleep(0.3)
    with tempfile.NamedTemporaryFile("w", suffix=".gr", prefix="netchk", delete=False) as f:
        f.write(PROGRAM.replace("@HOST@", host).replace("@ROUTER@", a.router))
        path = f.name
    try:
        out = grain_remote.run_file(a.pi, path, timeout=120)
    finally:
        stop.set()
        os.unlink(path)
    lines = out.splitlines()
    bad = 0
    for want, _ in EXPECT:
        want = want.replace("@HOST@", host)
        ok = want in lines
        bad += not ok
        print(f"  {'ok   ' if ok else 'WRONG'} {want}")
    extra = [l for l in lines if l not in [w.replace('@HOST@', host) for w, _ in EXPECT]]
    for l in extra:
        print(f"  board said: {l}")
    print(f"pi-net-check: {len(EXPECT) - bad} of {len(EXPECT)} as expected")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
