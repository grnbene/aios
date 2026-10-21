#!/usr/bin/env python3
"""Drive the SCREEN, not the serial port: boot the bare-metal image under UEFI
(no VGA text mode exists there), wait for the console banner on serial, then
take a framebuffer screenshot through the QEMU monitor and check that the
console actually rendered text — the top-left cell must be the '[' glyph of
"[metal]" exactly as our font draws it, and the screen must be mostly text.
Writes <out>.png for a human to look at. Usage: screenshot.py ISO OUT.png [--bios]"""
import os, sys, socket, subprocess, time, re, struct, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

iso, out = sys.argv[1], sys.argv[2]
uefi = "--bios" not in sys.argv
fw = firmware_args(force_uefi=uefi) if uefi else []
if not uefi and firmware_args(force_uefi=False) != []: print("SKIP: no BIOS GRUB here"); sys.exit(0)
work = os.path.dirname(os.path.abspath(out)) or "."
mon = os.path.join(work, "screenshot-monitor.sock"); ser = os.path.join(work, "screenshot-serial.log")
for f in (mon, ser):
    try: os.remove(f)
    except FileNotFoundError: pass
cmd = ["qemu-system-x86_64", *fw, "-m", "512", "-smp", "2", "-display", "none", "-vga", "std", "-no-reboot",
       "-cdrom", iso, "-boot", "d", "-serial", "file:" + ser, "-monitor", "unix:" + mon + ",server,nowait"]
if os.path.exists("/dev/kvm"): cmd += ["-enable-kvm", "-cpu", "host"]
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.time() + 240; banner = ""
    while time.time() < deadline:
        if os.path.exists(ser):
            banner = open(ser, "rb").read().decode("utf-8", "replace")
            if "type help" in banner: break
        if q.poll() is not None: print("FAIL: qemu exited early"); sys.exit(1)
        time.sleep(0.5)
    else: print("FAIL: no console banner on serial within 240 s"); sys.exit(1)
    m = re.search(r"\[metal\] display: (.*)", banner); print("serial says:", m.group(1) if m else "(no display line)")
    time.sleep(1.0)
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(5); time.sleep(0.3)
    try: s.recv(4096)
    except Exception: pass
    ppm = os.path.join(work, "screenshot.ppm")
    s.sendall(("screendump %s\n" % ppm).encode()); time.sleep(1.5)
    try: s.recv(4096)
    except Exception: pass
    for _ in range(20):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
        time.sleep(0.5)
    data = open(ppm, "rb").read()
    # PPM P6 header
    parts = data.split(maxsplit=4); w, h, maxv = int(parts[1]), int(parts[2]), int(parts[3]); pix = parts[4]
    print("screen: %dx%d" % (w, h))
    def px(x, y): i = (y * w + x) * 3; return pix[i], pix[i+1], pix[i+2]
    # PNG out (no PIL dependency)
    raw = b"".join(b"\x00" + pix[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d): c = t + d; return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    print("wrote", out)
    ok = True
    def check(cond, what):
        global ok; ok = ok and cond; print(("  ok    " if cond else "  FAIL  ") + what)
    # The kernel programs the adapter itself now, so there are two display
    # lines: the mode it SET, and the framebuffer it then reports. Match the
    # one being asserted rather than whichever came first.
    fbline = re.search(r"\[metal\] display: framebuffer (\d+)x(\d+)x(\d+)", banner)
    check(fbline is not None, "kernel reports a framebuffer console (%s)" %
          (fbline.group(0).split(": ", 1)[1] if fbline else "none"))
    # the top-left cell must be '[' as our font draws it (row 1 of the screen is the first banner line after the clear)
    font = {}
    for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "font8x16.h")):
        mm = re.match(r"\s*\{([0-9a-fx,]+)\},\s*// (\d+)", line)
        if mm: font[int(mm.group(2))] = [int(v, 16) for v in mm.group(1).split(",")]
    def cell_bits(cx, cy):
        bits = []
        for y in range(16):
            b = 0
            for x in range(8):
                r, g, bl = px(cx*8 + x, cy*16 + y)
                if r + g + bl > 300: b |= 0x80 >> x
            bits.append(b)
        return bits
    # find the first row whose first cell is not blank
    first = None
    for cy in range(h // 16):
        if any(cell_bits(0, cy)): first = cy; break
    check(first is not None, "something is drawn in the first column")
    if first is not None:
        got = cell_bits(0, first); want = font[ord("[")]
        check(got == want, "top-left glyph is '[' from our font (row %d)" % first)
        if got != want:
            for gy in range(16): print("    got " + "".join("#" if got[gy] & (0x80>>x) else "." for x in range(8)) + "  want " + "".join("#" if want[gy] & (0x80>>x) else "." for x in range(8)))
    lit = sum(1 for y in range(0, h, 4) for x in range(0, w, 4) if sum(px(x, y)) > 300)
    check(lit > 200, "the screen carries text (%d lit samples)" % lit)
    # the prompt: the last non-blank row should read "aios> "
    last = None
    for cy in range(h // 16 - 1, -1, -1):
        if any(cell_bits(0, cy)): last = cy; break
    if last is not None:
        rev = {tuple(v): chr(k) for k, v in font.items()}
        text = "".join(rev.get(tuple(cell_bits(cx, last)), "?") for cx in range(6))
        check(text.startswith("aios>"), "the last line on screen reads the prompt: %r" % text)
    # Now the keyboard: type "help" on the PS/2 keyboard through the monitor and
    # see the screen change — the help text lands, the prompt returns below it.
    def send_mon(cmd_str):
        try:
            s.sendall((cmd_str + "\n").encode())
            time.sleep(0.15)
            try: s.recv(4096)
            except Exception: pass
        except Exception: pass

    for key in ["h", "e", "l", "p", "ret"]:
        send_mon("sendkey %s" % key)
    time.sleep(1.5)
    ppm2 = os.path.join(work, "screenshot2.ppm")
    send_mon("screendump %s" % ppm2)
    time.sleep(1.5)
    for _ in range(20):
        if os.path.exists(ppm2) and os.path.getsize(ppm2) > 1000: break
        time.sleep(0.5)
    data2 = open(ppm2, "rb").read(); parts2 = data2.split(maxsplit=4); pix2 = parts2[4]
    out2 = out[:-4] + "-help.png" if out.endswith(".png") else out + "-help.png"
    raw2 = b"".join(b"\x00" + pix2[y*w*3:(y+1)*w*3] for y in range(h))
    open(out2, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw2, 6)) + chunk(b"IEND", b""))
    def px2(x, y): i = (y * w + x) * 3; return pix2[i], pix2[i+1], pix2[i+2]
    def cell_bits2(cx, cy):
        bits = []
        for y in range(16):
            b = 0
            for x in range(8):
                r, g, bl = px2(cx*8 + x, cy*16 + y)
                if r + g + bl > 300: b |= 0x80 >> x
            bits.append(b)
        return bits
    rows_lit = [cy for cy in range(h // 16) if any(any(cell_bits2(cx, cy)) for cx in range(0, min(w // 8, 40)))]
    last2 = rows_lit[-1] if rows_lit else None
    text2 = "".join(rev.get(tuple(cell_bits2(cx, last2)), "?") for cx in range(6)) if last2 is not None else ""
    check(last2 is not None and last2 > (last or 0) + 10, "typing 'help' on the keyboard filled the screen (last text row %s → %s)" % (last, last2))
    check(text2.startswith("aios>"), "and the prompt came back on the last line: %r" % text2)
    print("wrote", out2)
    print("DISPLAY: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
finally:
    q.kill()
