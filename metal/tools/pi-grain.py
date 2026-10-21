#!/usr/bin/env python3
"""Run a grain program on the Pi (bare-metal aios) and print its output.

    metal/tools/pi-grain.py FILE.gr [--pi <board ip>] [--name PROG.GR]

Uploads FILE to the SD card through the UDP console (grain_remote's push),
runs it with `grain run NAME OUT`, and prints what the board printed. The
console reply arrives as numbered UDP chunks ("[i/n] ..."); they are put back
in order. With --diff the same file is also run on the host
(`build/aios grain run`) and the two outputs are compared line by line.
"""
import argparse
import base64
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AIOS = os.path.abspath(os.path.join(HERE, "..", ".."))


# The transport and the upload are grain_remote's (at-most-once console,
# RAM-staged uploads), so there is one client, not two drifting apart.
sys.path.insert(0, os.path.join(AIOS, "tools"))
import grain_remote  # noqa: E402


def console(pi, cmd, quiet=None, total=120.0):
    return grain_remote.console(pi, cmd, total=total)


def upload(pi, name, data):
    import tempfile
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        path = f.name
    try:
        grain_remote.push(pi, path, name)
    finally:
        os.unlink(path)


def clean(text):
    out = []
    for line in text.splitlines():
        if line.startswith("  [output"):
            continue
        out.append(line[2:] if line.startswith("  ") else line)
    while out and not out[-1].strip():
        out.pop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--pi", default=os.environ.get("AIOS_PI", ""))
    ap.add_argument("--name", default=None, help="8.3 name on the card")
    ap.add_argument("--diff", action="store_true", help="also run on the host and compare")
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    base = re.sub(r"[^A-Z0-9]", "", os.path.splitext(os.path.basename(a.file))[0].upper())[:8] or "PROG"
    name = a.name or base + ".GR"
    out_name = base[:8] + ".OUT"
    upload(a.pi, name, data)
    t0 = time.time()
    reply = console(a.pi, f"grain run {name} {out_name}", quiet=1.5, total=300.0)
    pi_lines = clean(reply)
    print(f"--- pi ({time.time() - t0:.1f}s) ---")
    print("\n".join(pi_lines))
    if a.diff:
        host = subprocess.run([os.path.join(AIOS, "build", "aios"), "grain", "run", a.file],
                              capture_output=True, text=True, timeout=300)
        host_lines = [l.rstrip() for l in host.stdout.splitlines()]
        while host_lines and not host_lines[-1].strip():
            host_lines.pop()
        diffs = [(i, h, p) for i, (h, p) in enumerate(zip(host_lines, pi_lines)) if h.rstrip() != p.rstrip()]
        if len(host_lines) != len(pi_lines):
            diffs.append((min(len(host_lines), len(pi_lines)), f"<{len(host_lines)} lines>", f"<{len(pi_lines)} lines>"))
        print(f"--- host vs pi: {len(host_lines)} vs {len(pi_lines)} lines, {len(diffs)} differences ---")
        for i, h, p in diffs[:40]:
            print(f"  line {i + 1}:\n    host: {h}\n    pi:   {p}")
        sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
