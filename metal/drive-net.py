#!/usr/bin/env python3
"""Drive AOS's OWN TCP/IP stack on bare metal — no host network stack anywhere.
Boots the metal image under QEMU user-mode networking (SLIRP: DHCP, a gateway,
a DNS forwarder, NAT). Gates on the parts SLIRP always serves locally — a DHCP
lease and an ICMP ping to the gateway, which exercise Ethernet, ARP, IPv4, UDP,
DHCP and ICMP end to end. DNS, a real HTTP GET and a real Bitcoin node handshake
need outbound internet, so they are driven and reported but not required (a CI
sandbox may firewall them). Usage: drive-net.py [ISO]"""
import os, sys, pty, time, re, subprocess, select
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "metal", "aios-metal.iso")
mpty, spty = pty.openpty()
cmd = ["qemu-system-x86_64","-m","512","-smp","2","-nographic","-no-reboot","-cdrom",ISO,"-boot","d"] + firmware_args() + \
      ["-netdev","user,id=n0","-device",os.environ.get("AOS_NIC","virtio-net-pci")+",netdev=n0,mac=52:54:00:12:34:56"]   # AOS_NIC=e1000 drives the Intel driver instead
q = subprocess.Popen(cmd, stdin=spty, stdout=spty, stderr=subprocess.DEVNULL, close_fds=True)
os.close(spty)
buf = ""
def pump(t=1.0):
    global buf; end=time.time()+t
    while time.time()<end:
        r,_,_=select.select([mpty],[],[],0.2)
        if mpty in r:
            try: d=os.read(mpty,65536)
            except OSError: break
            if not d: break
            buf+=d.decode("utf-8","replace")
def wait(pat,t=180):
    end=time.time()+t
    while time.time()<end:
        if re.search(pat,buf): return True
        pump(0.5)
    return False
def cmd_(line,t=30):
    global buf; buf=""
    os.write(mpty,(line+"\r").encode()); wait(r"\naios> ",t)
    return re.sub(r"\x1b\[[0-9;=?]*[a-zA-Z]","",buf)
ok=True
def check(cond,what):
    global ok; ok=ok and cond; print(("  ok    " if cond else "  FAIL  ")+what)
try:
    assert wait(r"aios> ",240), "no prompt"
    up=cmd_("host net up",30); print(up.strip())
    check("up: ip 10.0.2.15" in up, "DHCP got a lease from SLIRP (Ethernet+ARP+IPv4+UDP+DHCP)")
    png=cmd_("host net ping 10.0.2.2",12); print(png.strip().splitlines()[-2] if png.strip() else "")
    check("reply from 10.0.2.2" in png, "ICMP echo to the gateway answered")
    # informational (need real internet):
    dns=cmd_("host net resolve seed.bitcoin.sipa.be",15)
    print("  info  DNS:", dns.strip().splitlines()[-2] if len(dns.strip().splitlines())>1 else "(no answer)")
    btc=cmd_("host net btc",90)
    hs = "handshake complete" in btc
    print("  " + ("ok    " if hs else "info  ") + "Bitcoin P2P handshake:", "COMPLETE with a real node" if hs else "(no egress to :8333 in this sandbox — run on a host with open internet)")
    for l in btc.splitlines():
        if "protocol version" in l or "user agent" in l or "block height" in l: print("        "+l.strip())
    print("NET: "+("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
finally:
    q.kill()
