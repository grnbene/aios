#!/usr/bin/env python3
"""Put a mind on the bare-metal machine's second serial port.

QEMU exposes COM2 as a unix socket (console.py start ... --mind SOCK, or a second
`-serial unix:SOCK,server,nowait`). This connects the Assay bridge's stdio to that
socket, so `mind bind mind wire` inside the machine finds a model. Model selection
as for the bridge: ~/.assay/config.json or AIOS_MODEL_PROVIDER / AIOS_MODEL (the
provider's key comes from the config's apiKeys). Keys never leave this host.

    metal/mind-wire.py /tmp/aios-mind.sock            # stays attached; Ctrl-C to detach
"""
import os, socket, subprocess, sys, threading, time

sock_path = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bridge = os.path.join(here, "intelligence", "assay", "bridge.cjs")
for _ in range(50):
    if os.path.exists(sock_path): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX); s.connect(sock_path)
p = subprocess.Popen(["node", bridge], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
print("wire: %s ↔ node %s (%s/%s)" % (sock_path, bridge, os.environ.get("AIOS_MODEL_PROVIDER", "config"), os.environ.get("AIOS_MODEL", "config")), file=sys.stderr, flush=True)

def machine_to_mind():
    first = True
    while True:
        d = s.recv(65536)
        if not d: break
        if first: print("wire: first bytes from the machine: %s" % d[:16].hex(), file=sys.stderr, flush=True); first = False
        # firmware/loader chatter may precede the first frame on a shared line; the bridge resynchronises on the magic
        try: p.stdin.write(d); p.stdin.flush()
        except BrokenPipeError: print("wire: the bridge exited (code %s)" % p.poll(), file=sys.stderr, flush=True); break
    try: p.stdin.close()
    except Exception: pass
def mind_to_machine():
    while True:
        d = p.stdout.read1(65536) if hasattr(p.stdout, "read1") else p.stdout.read(65536)
        if not d: break
        s.sendall(d)
t1 = threading.Thread(target=machine_to_mind, daemon=True); t2 = threading.Thread(target=mind_to_machine, daemon=True)
t1.start(); t2.start()
try:
    while t1.is_alive() and t2.is_alive(): time.sleep(0.5)
except KeyboardInterrupt: pass
p.kill()
