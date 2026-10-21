#!/usr/bin/env python3
"""Drive the bare-metal desktop's POWER CONTROL, on the metal, from the pixels.

Boots the image, enters `desktop`, then operates the power control the way a
person at the machine does — moves the real PS/2 mouse onto it, clicks, reads
the three option labels back out of video memory, closes the panel with Escape
and with a click outside, puts the machine to sleep and wakes it BOTH ways
(mouse and key), and finally chooses a real power option and proves the machine
acted on it.

Nothing here inspects kernel state. Every check is what the screen shows, plus
what the machine itself says on the serial line when it powers down.

Usage: drive-power.py ISO [OUTDIR]      env FINAL=poweroff|restart
"""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_common import firmware_args

iso = sys.argv[1]
work = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(iso))
FINAL = os.environ.get("FINAL", "poweroff")
os.makedirs(work, exist_ok=True)
qmp_path = os.path.join(work, "power-qmp.sock")
ser_path = os.path.join(work, "power-serial.sock")
for f in (qmp_path, ser_path):
    try:
        os.remove(f)
    except FileNotFoundError:
        pass

fw = firmware_args(force_uefi=True)
if not fw:
    print("SKIP: no OVMF firmware on this host")
    sys.exit(0)

cmd = ["qemu-system-x86_64", *fw, "-m", "1024", "-smp", "2", "-display", "none", "-vga", "std",
       "-no-reboot", "-cdrom", iso, "-boot", "d",
       "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_path, "-serial", "chardev:ser0",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
if os.path.exists("/dev/kvm"):
    cmd += ["-enable-kvm", "-cpu", "host"]
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ok = True


def check(cond, what):
    global ok
    ok = ok and bool(cond)
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)


def connect(path, deadline=60):
    end = time.time() + deadline
    while time.time() < end:
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            s.settimeout(10)
            return s
        except OSError:
            if q.poll() is not None:
                print("FAIL: qemu exited before its sockets appeared")
                sys.exit(1)
            time.sleep(0.3)
    print("FAIL: could not connect to " + path)
    sys.exit(1)


serial_buf = bytearray()


def serial_pump(sock, seconds=0.0):
    end = time.time() + seconds
    while True:
        sock.settimeout(0.2)
        try:
            b = sock.recv(65536)
            if not b:
                break
            serial_buf.extend(b)
        except socket.timeout:
            pass
        except OSError:
            break
        if time.time() >= end:
            break
    return serial_buf.decode("utf-8", "replace")


def serial_wait(sock, needle, timeout=240):
    end = time.time() + timeout
    while time.time() < end:
        text = serial_pump(sock, 0.5)
        if needle in text:
            return text
        if q.poll() is not None:
            return None
    return None


class Qmp:
    def __init__(self, path):
        self.s = connect(path)
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            m = json.loads(line)
            if "return" in m or "error" in m:
                if "error" in m:
                    raise RuntimeError("%s: %s" % (name, m["error"]))
                return m["return"]

    def events(self, evs):
        # A click on "Power off" or "Restart" takes the machine down between the
        # button going down and coming back up, so the monitor socket dying
        # here is the OUTCOME, not an error. The checks that follow read the
        # serial line and the exit status.
        try:
            self.cmd("input-send-event", events=evs)
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            pass

    def move(self, dx, dy):
        """Relative motion, the way the hardware carries it: one signed byte per
        axis per PS/2 packet, so a sweep is many packets."""
        step = 100
        while dx or dy:
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            dx -= sx
            dy -= sy
            evs = []
            if sx:
                evs.append({"type": "rel", "data": {"axis": "x", "value": sx}})
            if sy:
                evs.append({"type": "rel", "data": {"axis": "y", "value": sy}})
            if not evs:
                break
            self.events(evs)
            time.sleep(0.02)

    def click(self):
        self.events([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.15)
        self.events([{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.6)

    def key(self, qcode):
        self.events([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}},
                     {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}])


def parse_ppm(data):
    """Header, then EXACTLY one whitespace byte, then the raster.

    Splitting the file on whitespace instead eats any leading pixel byte that
    happens to BE whitespace — and 0x0A is both '\\n' and the desktop's own
    --bg, which is the colour the compositor draws the pointer's outline in. A
    pointer parked at (0,0) therefore shifted every pixel in the frame by one
    and made the arrow unfindable, which is not a screenshot problem, it is a
    parser problem."""
    if not data.startswith(b"P6"):
        return None
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if i < len(data) and data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        if start == i:
            return None
        fields.append(int(data[start:i]))
    i += 1                        # the single whitespace byte after maxval
    w, h = fields[0], fields[1]
    pix = data[i:]
    if len(pix) < w * h * 3:
        return None
    return w, h, pix


class Shot:
    """One frame of the machine's real video memory.

    QEMU returns from screendump before the file is finished, and half a PPM is
    not a picture of anything — so this waits for the size to settle AND for the
    pixel data to be complete, and asks again if it is not."""

    def __init__(self, qmp, tag):
        ppm = os.path.join(work, "power-%s.ppm" % tag)
        for attempt in range(4):
            try:
                os.remove(ppm)
            except FileNotFoundError:
                pass
            qmp.cmd("screendump", filename=ppm)
            last = -1
            for _ in range(120):
                time.sleep(0.2)
                if not os.path.exists(ppm):
                    continue
                size = os.path.getsize(ppm)
                if size > 1000 and size == last:
                    parsed = parse_ppm(open(ppm, "rb").read())
                    if parsed:
                        self.w, self.h, self.pix = parsed
                        self.tag = tag
                        return
                    break        # settled but short: take another screendump
                last = size
        raise SystemExit("FAIL: screendump %s never arrived complete" % tag)

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return self.pix[i], self.pix[i + 1], self.pix[i + 2]

    def lum(self, x, y):
        p = self.px(x, y)
        return (p[0] + p[1] + p[2]) / 3.0

    def row_runs(self, rgb, y, tol=10):
        runs, start = [], None
        for x in range(self.w):
            p = self.px(x, y)
            hit = all(abs(p[i] - rgb[i]) <= tol for i in range(3))
            if hit and start is None:
                start = x
            elif not hit and start is not None:
                runs.append((start, x - 1))
                start = None
        if start is not None:
            runs.append((start, self.w - 1))
        return runs

    def png(self, path):
        raw = b"".join(b"\x00" + self.pix[y * self.w * 3:(y + 1) * self.w * 3] for y in range(self.h))

        def chunk(t, d):
            c = t + d
            return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

        open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
                               + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
                               + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
        print("    wrote " + path)


# ---------------------------------------------------------------------------
# The kernel's own faces. The geometry under test is MEASURED text, so the
# drive measures with the same advances the kernel does — that is the only way
# to know where a label should be and how wide it should have made its row.
ATLAS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_atlas.json")))


def tw(text, face):
    f = ATLAS[face]
    return sum(f["glyphs"][str(ord(c))]["adv"] for c in text)


def stamp(text, face):
    """The coverage the kernel would lay down for `text`, as (w, h, oy, grid) —
    exactly the pen/bx/by walk draw_text_to does."""
    f = ATLAS[face]
    width = tw(text, face)
    top = min(f["glyphs"][str(ord(c))]["by"] for c in text if c != " ")
    bot = max(f["glyphs"][str(ord(c))]["by"] + f["glyphs"][str(ord(c))]["h"] for c in text if c != " ")
    h = bot - top
    grid = [[0] * width for _ in range(h)]
    pen = 0
    for c in text:
        g = f["glyphs"][str(ord(c))]
        for gy in range(g["h"]):
            for gx in range(g["w"]):
                x, y = pen + g["bx"] + gx, g["by"] + gy - top
                if 0 <= x < width and 0 <= y < h:
                    grid[y][x] = max(grid[y][x], g["pix"][gy * g["w"] + gx])
        pen += g["adv"]
    return width, h, top, grid


def reads_as(shot, x, y, text, face, thresh=0.80):
    """Does `text` in `face` appear with its pen origin at (x, y)? The ink is
    antialiased onto whatever surface is under it, so this correlates the
    screen's ink against the kernel's own coverage rather than comparing
    pixels. Returns the best correlation over a +/-1 px jitter."""
    width, h, top, grid = stamp(text, face)
    tnorm = sum(v * v for row in grid for v in row) ** 0.5
    best = 0.0
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            obs = []
            for ry in range(h):
                row = []
                for rx in range(width):
                    px_, py_ = x + dx + rx, y + top + dy + ry
                    row.append(0.0 if px_ >= shot.w or py_ >= shot.h or px_ < 0 or py_ < 0
                               else shot.lum(px_, py_))
                obs.append(row)
            floor = min(min(r) for r in obs)
            ink = [[max(0.0, v - floor) for v in row] for row in obs]
            norm = sum(v * v for row in ink for v in row) ** 0.5
            if norm == 0:
                continue
            dot = sum(ink[ry][rx] * grid[ry][rx] for ry in range(h) for rx in range(width))
            best = max(best, dot / (norm * tnorm))
    return best, best >= thresh


def ink_bbox(shot, x, y, w, h, min_lum=70):
    """The bounding box of TEXT ink inside a rect — how a label is PROVEN to fit
    the button it sits in: its ink has to stop before the edges do. The
    threshold is absolute so that the surface the label sits on (a dark panel,
    or the power control's own dark red fill) is not counted as ink."""
    lo_x, hi_x, lo_y, hi_y = None, None, None, None
    for j in range(h):
        for i in range(w):
            if shot.lum(x + i, y + j) >= min_lum:
                lo_x = i if lo_x is None else min(lo_x, i)
                hi_x = i if hi_x is None else max(hi_x, i)
                lo_y = j if lo_y is None else min(lo_y, j)
                hi_y = j if hi_y is None else max(hi_y, j)
    return lo_x, hi_x, lo_y, hi_y


# --- the geometry under test, computed the way power_geom() computes it ------
POWER_LABEL = "Power"
OPTIONS = [("Restart", "reset this machine"),
           ("Power off", "ACPI S5, off"),
           ("Sleep", "blank; any input wakes")]
SLEEP_HINT = "asleep — move the mouse, click, or press any key"
PANEL = (0x17, 0x17, 0x17)
STRONG = (0x76, 0x76, 0x76)


def geom(W):
    g = {}
    g["btn_w"] = tw(POWER_LABEL, "UI_BOLD") + 16
    g["btn_h"] = 22
    g["btn_x"] = W - g["btn_w"] - 10
    g["btn_y"] = 6
    pad, row_h = 6, 30
    widest = max(tw(l, "UI_BOLD") + 20 + tw(n, "UI") for l, n in OPTIONS)
    g["pad"], g["row_h"], g["rows"] = pad, row_h, len(OPTIONS)
    g["panel_w"] = widest + 24 + 2 * pad
    g["panel_h"] = len(OPTIONS) * row_h + 2 * pad
    g["panel_x"] = max(8, g["btn_x"] + g["btn_w"] - g["panel_w"])
    g["panel_y"] = 40
    g["row_x"] = g["panel_x"] + pad
    g["row_w"] = g["panel_w"] - 2 * pad
    g["row_y"] = lambda i: g["panel_y"] + pad + i * row_h
    return g


def panel_edges(shot, g):
    """Is the popover on screen, at exactly the width its strings asked for?

    Both vertical sides of the panel, in the outline colour, on the row through
    its middle — where the rounded corners are out of the way. This keys on the
    MEASURED panel width, so a panel that came out a different size than
    power_geom() computed does not pass."""
    my = g["panel_y"] + g["panel_h"] // 2

    def edge(x):
        return any(all(abs(shot.px(x + d, my)[i] - STRONG[i]) <= 14 for i in range(3))
                   for d in (0, 1, -1))
    return edge(g["panel_x"]) and edge(g["panel_x"] + g["panel_w"] - 1)


# The pointer the compositor draws, exactly as compositor.cpp draws it: 'X' is
# the dark outline, '.' the white fill. Finding it is how this drive knows where
# the machine thinks the mouse IS — a PS/2 mouse carries displacement, not a
# position, so open-loop "park and walk out" lands tens of pixels off and a
# 68px button is not a thing you can hit by dead reckoning.
CURSOR_ROWS = [
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
]


def find_pointer(shot):
    white = (0xFF, 0xFF, 0xFF)

    def dark(p):
        return p[0] < 0x40 and p[1] < 0x40 and p[2] < 0x40

    def matches(ox, oy):
        for ry, row in enumerate(CURSOR_ROWS):
            for rx, cell in enumerate(row):
                if cell == " ":
                    continue
                x, y = ox + rx, oy + ry
                if x >= shot.w or y >= shot.h:
                    return False
                p = shot.px(x, y)
                if cell == "X" and not dark(p):
                    return False
                if cell == "." and p != white:
                    return False
        return True

    for y in range(shot.h - len(CURSOR_ROWS)):
        for x in range(shot.w - 12):
            if shot.px(x, y) != white:
                continue
            ox, oy = x - 1, y - 2
            if ox >= 0 and oy >= 0 and matches(ox, oy):
                return (ox, oy)
    return None


goto_n = [0]


def goto(qmp, x, y, tol=3, tag=None):
    """Put the pointer ON (x, y), closed loop.

    A PS/2 mouse carries DISPLACEMENT, and this machine's driver accelerates it
    (metal/hal_metal.cpp scale_delta: 2x, 3x or 4x by magnitude), so "park at the
    corner and move 1876 right" throws the pointer off the far edge of the
    screen. The only reliable way to land on a 68px button is to read the arrow
    the kernel actually drew and step towards the target until it is there."""
    goto_n[0] += 1
    tag = tag or "goto%d" % goto_n[0]
    qmp.move(-3000, -3000)                # park at (0,0): the arrow is findable there
    time.sleep(0.5)
    at = None
    for i in range(14):
        at = find_pointer(Shot(qmp, "%s-%d" % (tag, i)))
        if at is None:
            # Lost it (clipped at an edge). Re-park and try again.
            qmp.move(-3000, -3000)
            time.sleep(0.5)
            continue
        dx, dy = x - at[0], y - at[1]
        if abs(dx) <= tol and abs(dy) <= tol:
            return at
        # The driver's coarsest gain is 4x; asking for a quarter of what is left
        # undershoots slightly and never overshoots off the screen.
        def step(n):
            if n == 0:
                return 0
            s = int(round(n / 4.0))
            return s if s else (1 if n > 0 else -1)
        qmp.move(step(dx), step(dy))
        time.sleep(0.4)
    print("    WARNING: pointer settled at %s, wanted (%d,%d)" % (at, x, y), flush=True)
    return at


try:
    ser = connect(ser_path)
    qmp = Qmp(qmp_path)

    print("waiting for the command surface on serial…", flush=True)
    banner = serial_wait(ser, "aios> ")
    if banner is None:
        print("FAIL: no console prompt on serial")
        sys.exit(1)
    m = re.search(r"\[metal\] display: framebuffer (\d+)x(\d+)x(\d+)", banner)
    check(m is not None, "the kernel has a linear framebuffer to composite onto")
    W, H = int(m.group(1)), int(m.group(2))
    print("  framebuffer %dx%d" % (W, H))

    print("\nentering the desktop…", flush=True)
    ser.sendall(b"desktop\n")
    dtext = serial_wait(ser, "[metal] desktop:", timeout=60)
    check(dtext is not None, "the desktop came up on the machine's own framebuffer")
    check(dtext is not None and "PS/2 mouse online" in dtext, "the PS/2 mouse came up")
    time.sleep(2.0)

    g = geom(W)
    print("  measured power control: %dx%d at (%d,%d); panel %dx%d at (%d,%d)"
          % (g["btn_w"], g["btn_h"], g["btn_x"], g["btn_y"],
             g["panel_w"], g["panel_h"], g["panel_x"], g["panel_y"]), flush=True)

    # ---- 1. the control is in the bar and says "Power" ---------------------
    s0 = Shot(qmp, "01-desktop")
    s0.png(os.path.join(work, "power-01-desktop.png"))
    score, hit = reads_as(s0, g["btn_x"] + 8, g["btn_y"] + 4, POWER_LABEL, "UI_BOLD")
    check(hit, "the top bar's power control reads \"Power\" (correlation %.2f)" % score)
    # The pill's own fill is dark red; only the label is bright, so a bright-ink
    # bbox is the label and nothing else.
    bx, hx, _, _ = ink_bbox(s0, g["btn_x"], g["btn_y"], g["btn_w"], g["btn_h"], min_lum=150)
    check(bx is not None and bx >= 2 and hx <= g["btn_w"] - 3,
          "its label fits inside the control: ink spans x=%s..%s of a %dpx button"
          % (bx, hx, g["btn_w"]))
    check(not panel_edges(s0, g), "no panel is on screen before it is asked for")

    # ---- 2. clicking it OPENS the panel, with three readable rows ----------
    print("\nclicking the power control with the real mouse…", flush=True)
    goto(qmp, g["btn_x"] + g["btn_w"] // 2, g["btn_y"] + g["btn_h"] // 2)
    qmp.click()
    s1 = Shot(qmp, "02-open")
    s1.png(os.path.join(work, "power-02-open.png"))
    check(panel_edges(s1, g),
          "a panel opened, with both its sides exactly where the measured strings put them"
          " (x=%d and x=%d)" % (g["panel_x"], g["panel_x"] + g["panel_w"] - 1))
    inside = s1.px(g["panel_x"] + g["panel_w"] // 2, g["panel_y"] + g["panel_h"] // 2)
    check(all(abs(inside[i] - PANEL[i]) <= 14 for i in range(3)),
          "the panel is a real opaque surface over the desktop (fill %s)" % (inside,))

    for i, (label, note) in enumerate(OPTIONS):
        ry = g["row_y"](i)
        ty = ry + (g["row_h"] - 16) // 2
        sc, hit = reads_as(s1, g["row_x"] + 12, ty, label, "UI_BOLD")
        check(hit, "row %d is labelled \"%s\" (correlation %.2f)" % (i + 1, label, sc))
        nx = g["row_x"] + g["row_w"] - 12 - tw(note, "UI")
        sc2, hit2 = reads_as(s1, nx, ty, note, "UI", thresh=0.72)
        check(hit2, "  and says what it does: \"%s\" (correlation %.2f)" % (note, sc2))
        lo_x, hi_x, lo_y, hi_y = ink_bbox(s1, g["row_x"], ry, g["row_w"], g["row_h"])
        check(lo_x is not None and lo_x >= 4 and hi_x <= g["row_w"] - 5
              and lo_y is not None and lo_y >= 1 and hi_y <= g["row_h"] - 2,
              "  and every pixel of it fits the row: ink x=%s..%s y=%s..%s in a %dx%d row"
              % (lo_x, hi_x, lo_y, hi_y, g["row_w"], g["row_h"]))

    # ---- 3. Escape closes it ----------------------------------------------
    print("\npressing Escape…", flush=True)
    qmp.key("esc")
    time.sleep(0.8)
    s2 = Shot(qmp, "03-escape")
    s2.png(os.path.join(work, "power-03-escape.png"))
    check(not panel_edges(s2, g), "Escape closed the panel")
    sc, hit = reads_as(s2, g["btn_x"] + 8, g["btn_y"] + 4, POWER_LABEL, "UI_BOLD")
    check(hit, "and the desktop is still there with its power control (Escape did not exit)")

    # ---- 4. a click outside closes it -------------------------------------
    print("\nre-opening, then clicking out in the middle of the screen…", flush=True)
    goto(qmp, g["btn_x"] + g["btn_w"] // 2, g["btn_y"] + g["btn_h"] // 2)
    qmp.click()
    s3 = Shot(qmp, "04-reopen")
    check(panel_edges(s3, g), "the panel opened again")
    goto(qmp, W // 3, H // 2)
    qmp.click()
    s4 = Shot(qmp, "05-clickaway")
    s4.png(os.path.join(work, "power-05-clickaway.png"))
    check(not panel_edges(s4, g), "a click outside closed it")

    # ---- 5. Sleep, and wake with the MOUSE --------------------------------
    print("\nchoosing Sleep…", flush=True)
    goto(qmp, g["btn_x"] + g["btn_w"] // 2, g["btn_y"] + g["btn_h"] // 2)
    qmp.click()
    sleep_row = 2
    goto(qmp, g["row_x"] + g["row_w"] // 2, g["row_y"](sleep_row) + g["row_h"] // 2)
    qmp.click()
    time.sleep(1.5)
    s5 = Shot(qmp, "06-asleep")
    s5.png(os.path.join(work, "power-06-asleep.png"))
    lit = sum(1 for y in range(0, s5.h, 4) for x in range(0, s5.w, 4) if s5.lum(x, y) > 24)
    total = len(range(0, s5.h, 4)) * len(range(0, s5.w, 4))
    check(lit * 100 < total, "the screen went dark: %d of %d sampled pixels above black" % (lit, total))
    hint_w = tw(SLEEP_HINT.replace("—", "-"), "UI")
    sc, hit = reads_as(s5, (s5.w - hint_w) // 2, s5.h // 2 - 8, SLEEP_HINT.replace("—", "-"),
                       "UI", thresh=0.60)
    check(hit, "and it says it is asleep and how to wake it (correlation %.2f)" % sc)

    print("\nmoving the mouse to wake it…", flush=True)
    qmp.move(-40, -30)
    time.sleep(1.2)
    s6 = Shot(qmp, "07-woke-mouse")
    s6.png(os.path.join(work, "power-07-woke-mouse.png"))
    sc, hit = reads_as(s6, g["btn_x"] + 8, g["btn_y"] + 4, POWER_LABEL, "UI_BOLD")
    check(hit, "mouse motion woke it: the live desktop is back with its top bar")
    check(not panel_edges(s6, g), "and the wake did not leave the power panel open")

    # ---- 6. Sleep again, and wake with a KEY ------------------------------
    print("\nsleeping again, waking with a keypress…", flush=True)
    goto(qmp, g["btn_x"] + g["btn_w"] // 2, g["btn_y"] + g["btn_h"] // 2)
    qmp.click()
    goto(qmp, g["row_x"] + g["row_w"] // 2, g["row_y"](sleep_row) + g["row_h"] // 2)
    qmp.click()
    time.sleep(1.5)
    s7 = Shot(qmp, "08-asleep2")
    lit = sum(1 for y in range(0, s7.h, 4) for x in range(0, s7.w, 4) if s7.lum(x, y) > 24)
    check(lit * 100 < total, "asleep again (%d of %d sampled pixels above black)" % (lit, total))
    qmp.key("a")                       # a plain key, not Escape
    time.sleep(1.2)
    s8 = Shot(qmp, "09-woke-key")
    s8.png(os.path.join(work, "power-09-woke-key.png"))
    sc, hit = reads_as(s8, g["btn_x"] + 8, g["btn_y"] + 4, POWER_LABEL, "UI_BOLD")
    check(hit, "a keypress woke it too: the live desktop is back")

    # ---- 7. a real power option -------------------------------------------
    row = 1 if FINAL == "poweroff" else 0
    want = "[metal] powering off" if FINAL == "poweroff" else "[metal] rebooting"
    print("\nchoosing \"%s\"…" % OPTIONS[row][0], flush=True)
    goto(qmp, g["btn_x"] + g["btn_w"] // 2, g["btn_y"] + g["btn_h"] // 2)
    qmp.click()
    goto(qmp, g["row_x"] + g["row_w"] // 2, g["row_y"](row) + g["row_h"] // 2)
    qmp.click()
    said = serial_wait(ser, want, timeout=40)
    check(said is not None, "the machine itself reported %r on its serial line" % want)
    end = time.time() + 30
    while time.time() < end and q.poll() is None:
        time.sleep(0.5)
    check(q.poll() is not None,
          "and the machine actually went down (qemu exited %s)" % q.poll())

finally:
    if q.poll() is None:
        q.kill()
    print("\n" + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
