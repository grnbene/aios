#!/usr/bin/env python3
"""Timestamp every aios breadcrumb broadcast (udp/42425) into /tmp/pi-crumbs.log."""
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.bind(("0.0.0.0", 42425))
with open("/tmp/pi-crumbs.log", "a", buffering=1) as f:
    while True:
        data, _ = s.recvfrom(2048)
        now = time.time()
        f.write("%s.%03d %s\n" % (time.strftime("%H:%M:%S", time.localtime(now)),
                                  int((now % 1) * 1000), data.decode(errors="replace")))
