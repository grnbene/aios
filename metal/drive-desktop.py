#!/usr/bin/env python3
"""Drive the bare-metal GRAPHICAL DESKTOP: boot the image, enter `desktop` on
the command surface, then operate it the way a person at the machine does —
move the real PS/2 mouse, click and drag a titlebar, click a chip, type a
command on the keyboard — and prove each one from the pixels the kernel put in
video memory. Nothing here inspects kernel state: every check is what the
screen shows.

Usage: drive-desktop.py ISO [OUTDIR]
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
os.makedirs(work, exist_ok=True)
qmp_path = os.path.join(work, "desktop-qmp.sock")
ser_path = os.path.join(work, "desktop-serial.sock")
for f in (qmp_path, ser_path):
    try:
        os.remove(f)
    except FileNotFoundError:
        pass

fw = firmware_args(force_uefi=True)
if not fw:
    print("SKIP: no OVMF firmware on this host")
    sys.exit(0)

# Which display the machine gets. "std" is a Bochs-class adapter: the loader
# has a GOP for it and the kernel can also program it directly. "virtio-gpu" is
# the other case entirely — no VGA compatibility, no DISPI registers, and on
# this firmware no GOP either, so the kernel has to bring the display up
# through the device's own queues or run blind.
VGA = os.environ.get("AIOS_VGA", "std")
vga_args = (["-vga", "std"] if VGA == "std" else
            ["-vga", "none", "-device", "%s,xres=1920,yres=1080" % VGA])

cmd = ["qemu-system-x86_64", *fw, "-m", "1024", "-smp", "2", "-display", "none", *vga_args,
       "-no-reboot", "-cdrom", iso, "-boot", "d",
       "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_path, "-serial", "chardev:ser0",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
if os.path.exists("/dev/kvm"):
    cmd += ["-enable-kvm", "-cpu", "host"]
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ok = True
notes = []


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
            print("FAIL: qemu exited while waiting for %r" % needle)
            sys.exit(1)
    return None


class Qmp:
    def __init__(self, path):
        self.s = connect(path)
        self.f = self.s.makefile("rwb")
        self.f.readline()          # greeting
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
        self.cmd("input-send-event", events=evs)

    def move(self, dx, dy):
        """Relative motion, the way the hardware carries it. A PS/2 packet has
        one signed byte per axis, so a big sweep is many packets — exactly what
        a hand moving the mouse produces."""
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

    def button(self, down, which="left"):
        self.events([{"type": "btn", "data": {"down": down, "button": which}}])

    def key(self, qcode):
        self.events([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}},
                     {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}])


class Shot:
    """One frame of the machine's real video memory."""

    def __init__(self, qmp, tag):
        ppm = os.path.join(work, "desk-%s.ppm" % tag)
        try:
            os.remove(ppm)
        except FileNotFoundError:
            pass
        qmp.cmd("screendump", filename=ppm)
        # Wait for the whole frame: QEMU returns from screendump before the
        # file is finished, and half a PPM is not a picture of anything.
        data = b""
        for _ in range(80):
            time.sleep(0.25)
            if not os.path.exists(ppm):
                continue
            try:
                data = open(ppm, "rb").read()
                lines_found, idx = 0, 0
                while idx < len(data) and lines_found < 3:
                    if data[idx] == 10:
                        lines_found += 1
                    idx += 1
                if lines_found == 3:
                    hdr = data[:idx].decode("ascii", "replace").split()
                    self.w, self.h = int(hdr[1]), int(hdr[2])
                    self.pix = data[idx:]
                    want = self.w * self.h * 3
                    if len(self.pix) >= want:
                        break
            except Exception:
                pass
        self.tag = tag
        lines_found, idx = 0, 0
        while idx < len(data) and lines_found < 3:
            if data[idx] == 10:
                lines_found += 1
            idx += 1
        if lines_found == 3:
            hdr = data[:idx].decode("ascii", "replace").split()
            self.w, self.h = int(hdr[1]), int(hdr[2])
            self.pix = data[idx:]
        else:
            self.w, self.h, self.pix = 0, 0, b""
        want = self.w * self.h * 3
        if len(self.pix) < want:
            raise SystemExit("FAIL: screendump %s truncated (%d of %d bytes)" % (tag, len(self.pix), want))

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return self.pix[i], self.pix[i + 1], self.pix[i + 2]

    def colours(self, step=3):
        return {self.px(x, y) for y in range(0, self.h, step) for x in range(0, self.w, step)}

    def count(self, rgb, tol=12):
        n = 0
        for y in range(self.h):
            for x in range(self.w):
                p = self.px(x, y)
                if abs(p[0] - rgb[0]) <= tol and abs(p[1] - rgb[1]) <= tol and abs(p[2] - rgb[2]) <= tol:
                    n += 1
        return n

    def row_runs(self, rgb, y, tol=12):
        """x-ranges on row y whose pixels match rgb: window borders show up here."""
        runs, start = [], None
        for x in range(self.w):
            p = self.px(x, y)
            hit = abs(p[0] - rgb[0]) <= tol and abs(p[1] - rgb[1]) <= tol and abs(p[2] - rgb[2]) <= tol
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


# The kernel's own faces, so the drive can READ what is on the screen rather
# than counting coloured pixels and hoping. The desktop no longer draws with a
# 1-bit ROM font: metal/tools/mkfont.py rasterises real outlines into 8-bit
# coverage, the kernel blends it, and so a glyph on screen is a GREY ramp on
# whatever surface is under it. Matching it therefore means correlating against
# the same coverage the kernel drew, not comparing a bitmap for equality.
ATLAS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_atlas.json")
ATLAS = json.load(open(ATLAS_PATH))
MONO = ATLAS["MONO"]
MONO_ADV = MONO["glyphs"][str(ord("m"))]["adv"]


def mono_templates():
    """Per character, the coverage the kernel would lay in one mono cell."""
    out = {}
    cell_w, cell_h = MONO_ADV, MONO["line_h"] + 2
    for code, g in MONO["glyphs"].items():
        ch = chr(int(code))
        cell = [[0] * cell_w for _ in range(cell_h)]
        for gy in range(g["h"]):
            for gx in range(g["w"]):
                x, y = gx + g["bx"], gy + g["by"]
                if 0 <= x < cell_w and 0 <= y < cell_h:
                    cell[y][x] = g["pix"][gy * g["w"] + gx]
        out[ch] = cell
    return out, cell_w, cell_h


MONO_CELLS, CELL_W, CELL_H = mono_templates()


def read_line_at(shot, x, y, cells=90):
    """Decode one row of the compositor's monospaced text at (x, y).

    Each cell is scored against every glyph's coverage by correlation, because
    the ink is antialiased and sits on a surface whose own colour varies: the
    absolute pixel values are not the kernel's coverage, only proportional to
    it. The best-correlating glyph wins, and a cell with no ink at all is a
    space."""
    text = ""
    for c in range(cells):
        # Lift the cell's ink out of its background: the surface is dark and
        # the text light, so ink is how far a pixel is above the cell's floor.
        obs = []
        peak = 0
        for ry in range(CELL_H):
            row = []
            for rx in range(CELL_W):
                px_, py_ = x + c * CELL_W + rx, y + ry
                if px_ >= shot.w or py_ >= shot.h:
                    row.append(0)
                    continue
                p = shot.px(px_, py_)
                v = (p[0] + p[1] + p[2]) / 3.0
                row.append(v)
                peak = max(peak, v)
            obs.append(row)
        floor = min(min(r) for r in obs)
        # Ink bleeding in from the glyph next door is dim: a real glyph, even a
        # full stop, has pixels near full contrast. Below that, the cell is a
        # space — otherwise every gap beside a wide letter decodes as '?'.
        if peak - floor < 60:
            text += " "
            continue
        ink = [[max(0.0, v - floor) for v in row] for row in obs]
        norm = sum(v * v for row in ink for v in row) ** 0.5
        best, best_score = "?", 0.0
        for ch, cell in MONO_CELLS.items():
            tnorm = sum(v * v for row in cell for v in row) ** 0.5
            if tnorm == 0:
                continue
            dot = 0.0
            for ry in range(CELL_H):
                orow, trow = ink[ry], cell[ry]
                for rx in range(CELL_W):
                    dot += orow[rx] * trow[rx]
            score = dot / (norm * tnorm)
            if score > best_score:
                best, best_score = ch, score
        text += best if best_score > 0.72 else "?"
    return text.rstrip()


def read_window_text(shot, frame_left, frame_top, rows=24):
    """Every text row a window drew. The terminal lays its lines out on an 18px
    pitch starting 36px into the content area, and the content starts 26px below
    the window's own top edge — but the edge the border scan reports can be a
    scanline out, and one scanline out turns every glyph into a question mark.
    So the origin is tuned by reading and keeping whichever offset decodes."""
    best = None
    for dx in (-1, 0, 1):
        for dy in (-2, -1, 0, 1, 2):
            x = frame_left + 11 + dx
            y = frame_top + 62 + dy
            got = [read_line_at(shot, x, y + i * 18) for i in range(rows)]
            body = "".join(got)
            score = body.count("?") - sum(1 for c in body if c.isalnum())
            if best is None or score < best[0]:
                best = (score, got)
    return best[1]


# The focused window's frame. The desktop wears the assay console's palette, so
# this is --border-strong (#767676); an unfocused frame is --border (#3d3d3d),
# far enough away to tell apart at the 12-per-channel tolerance row_runs uses.
FOCUS_BORDER = (0x38, 0x8B, 0xFD)

# The pointer the compositor draws, exactly as src/userland/compositor.cpp
# draws it: 'X' is the dark outline, '.' the white fill.
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


def frame_columns(shot, rgb=FOCUS_BORDER):
    """Columns where the frame colour runs VERTICALLY for a long way — which is
    what a window's side is.

    The desktop's text is antialiased now, so a glyph's edge passes through
    every grey between its surface and its ink, including this one. A single
    matching pixel therefore proves nothing: a row of ordinary text contains
    dozens of them, spread right across a pane, and keying on "two matches far
    apart on one scanline" found the middle of a paragraph and called it a
    window. Only a line 60 pixels long is a frame."""
    def hit(x, y):
        p = shot.px(x, y)
        return abs(p[0] - rgb[0]) <= 12 and abs(p[1] - rgb[1]) <= 12 and abs(p[2] - rgb[2]) <= 12

    top_lim, bot_lim = 36, shot.h - 40
    candidates = set()
    for y in range(top_lim, bot_lim, 6):
        for start, end in shot.row_runs(rgb, y):
            candidates.add(start)
            candidates.add(end)
    cols = {}
    for x in sorted(candidates):
        y = top_lim
        while y < bot_lim:
            if not hit(x, y):
                y += 1
                continue
            run_top = y
            while y < bot_lim and hit(x, y):
                y += 1
            if y - run_top >= 60:
                cols.setdefault(x, (run_top, y - run_top))
    return cols


def focused_frame(shot):
    """The frame of the window that has focus: only that one gets the bright
    border, so this is the screen's own answer to "what am I typing into".
    Sides that start on the same scanline and run the same distance belong to
    the same window; the pair that is more than 200px apart is its frame."""
    cols = frame_columns(shot)
    groups = {}
    for x, (top, length) in cols.items():
        key = (round(top / 4.0), round(length / 8.0))
        groups.setdefault(key, []).append(x)
    def hit(x, y):
        p = shot.px(x, y)
        return (abs(p[0] - FOCUS_BORDER[0]) <= 12 and abs(p[1] - FOCUS_BORDER[1]) <= 12
                and abs(p[2] - FOCUS_BORDER[2]) <= 12)

    best = None
    for (top_k, _), xs in groups.items():
        if len(xs) < 2:
            continue
        left, right = min(xs), max(xs)
        if right - left <= 200:
            continue
        side_top = min(cols[x][0] for x in xs)
        # A side does not start at the window's top edge: the corners are
        # rounded, so the leftmost column only joins the frame a radius below
        # it. The TOP BORDER is a full-width row, so it is still there in the
        # middle of the window — and that row is the window's own top edge.
        # Taking the side's start instead put the text origin nine pixels out,
        # which decodes as a screen full of question marks.
        mid = (left + right) // 2
        top = side_top
        for y in range(max(0, side_top - 40), side_top + 1):
            if hit(mid, y):
                top = y
                break
        if best is None or (right - left) > (best[2] - best[1]):
            best = (top, left, right)
    return best


def find_pointer(shot):
    """Where the pointer is on screen, by matching the arrow the kernel draws.
    Returns its hotspot (the tip), which is the mouse position itself."""
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

    # The fill is pure white and rare on this desktop, so every white pixel is a
    # cheap candidate for the arrow's second row.
    for y in range(shot.h - len(CURSOR_ROWS)):
        for x in range(shot.w - 12):
            if shot.px(x, y) != white:
                continue
            ox, oy = x - 1, y - 2
            if ox >= 0 and oy >= 0 and matches(ox, oy):
                return (oy, ox)
    return None

try:
    ser = connect(ser_path)
    qmp = Qmp(qmp_path)

    print("waiting for the command surface on serial…", flush=True)
    banner = serial_wait(ser, "aios> ")
    if banner is None:
        print("FAIL: no console prompt on serial")
        sys.exit(1)
    # Two display lines now, and they say different things: the adapter mode the
    # KERNEL programmed, and the framebuffer it ended up compositing onto.
    setmode = re.search(r"\[metal\] display: adapter mode set to (\d+)x(\d+)", banner)
    m = re.search(r"\[metal\] display: framebuffer (\d+)x(\d+)x(\d+)", banner)
    print("  display:", m.group(0).strip() if m else "(not reported)")
    check(m is not None, "the kernel has a linear framebuffer to composite onto")
    if setmode:
        print("  mode set by the kernel:", setmode.group(1) + "x" + setmode.group(2))
    gpu = re.search(r"\[metal\] display: virtio-gpu scanout (\d+)x(\d+)", banner)
    if VGA != "std":
        # Nothing in the boot path could give this machine a framebuffer: the
        # kernel had to drive the device itself to have a screen at all.
        check(gpu is not None,
              "the kernel brought the virtio-gpu up through its own queues"
              + ("" if not gpu else " (%sx%s scanout)" % (gpu.group(1), gpu.group(2))))
        check(m is not None and gpu is not None and m.group(1) == gpu.group(1),
              "and composites onto the scanout it created")
    else:
        got = setmode or m
        check(got is not None and int(got.group(1)) >= 1280,
              "the machine came up at a real resolution, not the firmware's 1024x768 floor"
              + ("" if not got else " (%sx%s)" % (got.group(1), got.group(2))))

    before = Shot(qmp, "console")
    print("\nentering the desktop…", flush=True)
    ser.sendall(b"desktop\n")
    dtext = serial_wait(ser, "[metal] desktop:", timeout=60)
    if dtext is None:
        print("FAIL: the desktop never announced itself")
        sys.exit(1)
    dm = re.search(r"\[metal\] desktop: framebuffer (\S+), PS/2 mouse (\w+)", dtext)
    print("  desktop:", dm.group(0) if dm else "(unparsed)")
    check(dm is not None, "the desktop reported the framebuffer it composites onto")
    check(dm is not None and dm.group(2) == "online",
          "the PS/2 mouse came up: the AUX port answered the enable sequence")
    time.sleep(2.0)
    qmp.key("f2")
    time.sleep(1.0)

    s1 = Shot(qmp, "desktop")
    s1.png(os.path.join(work, "desktop-01-opened.png"))
    check(len(s1.colours()) > 12,
          "the screen is a composited desktop, not a text console (%d distinct colours, console had %d)"
          % (len(s1.colours()), len(before.colours())))
    sides = frame_columns(s1)
    check(len(sides) >= 2,
          "a focused window is on screen with its border drawn (%d long vertical sides in the focus colour)"
          % len(sides))

    # ---- the mouse: move it and find the pointer where it should be ----------
    print("\nmoving the real PS/2 mouse…", flush=True)
    qmp.move(-2000, -2000)          # park hard against the top-left corner
    time.sleep(1.0)
    qmp.move(300, 240)
    time.sleep(1.5)
    s2 = Shot(qmp, "mouse")
    s2.png(os.path.join(work, "desktop-02-pointer.png"))

    p2 = find_pointer(s2)
    print("  pointer found at", p2)
    check(p2 is not None, "the software pointer is drawn on the framebuffer")
    if p2:
        py, px_ = p2
        check(abs(px_ - 300) < 40 and abs(py - 240) < 40,
              "it landed where the mouse was moved to (~300,240 → %d,%d): the PS/2 packets carry real displacement"
              % (px_, py))

    # ---- drag a window by its titlebar --------------------------------------
    print("\ndragging a window by its titlebar…", flush=True)
    # One measurement of the frame, from focused_frame, which knows the corners
    # are rounded. Measuring it inline here instead is how the text read ended
    # up 6px out and decoded every glyph as '?'.
    grabbed = focused_frame(s1)
    top_row = grabbed[0] if grabbed else None
    left_edge, right_edge = (grabbed[1], grabbed[2]) if grabbed else (0, 0)
    check(top_row is not None, "found the focused window's frame to grab")
    if top_row is not None:
        print("  window frame: top y=%d, x %d..%d" % (top_row, left_edge, right_edge))
        grab_x, grab_y = left_edge + 120, top_row + 12
        qmp.move(-2000, -2000)
        time.sleep(0.8)
        qmp.move(grab_x, grab_y)
        time.sleep(1.0)
        pre = Shot(qmp, "pre-drag")
        print("  pointer parked at", find_pointer(pre), "(wanted %d,%d)" % (grab_x, grab_y))
        qmp.button(True)
        time.sleep(0.5)
        for _ in range(6):
            qmp.move(20, 12)
            time.sleep(0.12)
        time.sleep(0.6)
        qmp.button(False)
        time.sleep(1.2)
        s3 = Shot(qmp, "drag")
        s3.png(os.path.join(work, "desktop-03-dragged.png"))
        moved = focused_frame(s3)
        new_top = moved[0] if moved else None
        new_left = moved[1] if moved else None
        print("  window frame now: top y=%s, left x=%s" % (new_top, new_left if new_top else None))
        check(new_top is not None and (new_top > top_row + 30) and (new_left > left_edge + 60),
              "the window followed the mouse: it moved down-right by (%s,%s) px"
              % (None if new_top is None else new_left - left_edge,
                 None if new_top is None else new_top - top_row))

    # ---- type a command on the keyboard, into the desktop's terminal --------
    print("\ntyping a command at the desktop…", flush=True)
    for qcode in ["i", "n", "f", "o", "ret"]:
        qmp.key(qcode)
        time.sleep(0.12)
    time.sleep(2.5)
    s4 = Shot(qmp, "typed")
    s4.png(os.path.join(work, "desktop-04-command.png"))
    # Read the terminal's lines straight off the framebuffer, in the kernel's
    # own font: this is what a person at the machine is looking at.
    term_left, term_top = (new_left, new_top) if top_row is not None and new_top is not None else (left_edge, top_row)
    lines = [l for l in read_window_text(s4, term_left, term_top) if l.strip()]
    print("  the terminal window reads:")
    for l in lines:
        print("    | " + l)
    echoed = any(l.strip().startswith("aios> info") for l in lines)
    check(echoed, "the keystrokes reached the desktop's terminal: it echoes the typed command")
    # `info` answers with this machine's own topology; the arch string is the
    # substrate talking, not the compositor.
    ran = any("bare-metal" in l for l in lines) or any("Kernel ID" in l for l in lines)
    check(ran, "the command ran on the real command surface and its answer rendered in the window")

    # ---- click a chip: the mouse runs a command ----------------------------
    print("\nclicking the terminal's 'devices' chip with the mouse…", flush=True)
    # The chips are drawn from src/userland/compositor.cpp's TERM_CHIPS, at
    # content y 4..24, starting at content x 8 with 6px between them.
    chips = ["info", "ps", "devices", "free", "tasks", "tree /", "clear"]

    def ui_width(text):
        """What text_width(text, Face::Ui) returns in the kernel."""
        g = ATLAS["UI"]["glyphs"]
        return sum(g[str(ord(c))]["adv"] for c in text)

    chip_x = 8
    for name in chips:
        if name == "devices":
            break
        chip_x += ui_width(name) + 18 + 6
    target_x = term_left + 1 + chip_x + 20
    target_y = term_top + 26 + 14
    qmp.move(-2000, -2000)
    time.sleep(0.8)
    qmp.move(target_x, target_y)
    time.sleep(0.8)
    qmp.button(True)
    time.sleep(0.3)
    qmp.button(False)
    time.sleep(2.5)
    s6 = Shot(qmp, "chip")
    s6.png(os.path.join(work, "desktop-06-chip-click.png"))
    chip_lines = [l for l in read_window_text(s6, term_left, term_top) if l.strip()]
    print("  after the click the terminal reads:")
    for l in chip_lines[-10:]:
        print("    | " + l)
    check(any(l.strip().startswith("aios> devices") for l in chip_lines),
          "the click landed on the chip and ran its command")
    check(any("cpu(" in l or "lanes" in l for l in chip_lines),
          "the device listing the kernel produced rendered in the window")

    # ---- the wheel scrolls the terminal back -------------------------------
    print("\nscrolling the terminal with the mouse wheel…", flush=True)
    # There has to BE a backlog before scrolling one means anything. At 1080p
    # the shell pane is tall enough to hold everything typed so far, so the
    # wheel had nothing above it to reach and the check failed on a terminal
    # that was working perfectly. `help` is the longest thing the machine says.
    for qcode in ["h", "e", "l", "p", "ret"]:
        qmp.key(qcode)
        time.sleep(0.12)
    time.sleep(3.0)
    s6b = Shot(qmp, "backlog")
    top_before = next((l for l in read_window_text(s6b, term_left, term_top) if l.strip()), "")
    for _ in range(4):
        qmp.events([{"type": "btn", "data": {"down": True, "button": "wheel-up"}},
                    {"type": "btn", "data": {"down": False, "button": "wheel-up"}}])
        time.sleep(0.2)
    time.sleep(1.5)
    s7 = Shot(qmp, "wheel")
    s7.png(os.path.join(work, "desktop-07-wheel.png"))
    top_after = next((l for l in read_window_text(s7, term_left, term_top) if l.strip()), "")
    print("  first visible line: %r -> %r" % (top_before[:48], top_after[:48]))
    check(top_after != top_before, "the wheel scrolled the terminal's backlog")

    # ---- a function key raises another app --------------------------------
    print("\npressing F2 to raise the Machine pane…", flush=True)
    before_focus = focused_frame(s7)
    qmp.key("f2")
    time.sleep(2.0)
    s8 = Shot(qmp, "fkey")
    s8.png(os.path.join(work, "desktop-08-f2-machine.png"))
    after_focus = focused_frame(s8)
    print("  focused frame: %s -> %s" % (before_focus, after_focus))
    check(after_focus is not None and before_focus is not None and after_focus[1] != before_focus[1],
          "F2 moved focus to a different window: the keyboard drives the window manager")

    # ---- leave, and land back on the command surface ------------------------
    print("\nleaving the desktop with Escape…", flush=True)
    qmp.key("esc")
    left = serial_wait(ser, "the command surface has the screen back", timeout=30)
    check(left is not None, "Escape left the desktop and gave the screen back to the console")
    time.sleep(1.5)
    s5 = Shot(qmp, "back")
    s5.png(os.path.join(work, "desktop-05-back-to-console.png"))
    check(len(s5.colours()) < len(s1.colours()),
          "the screen is a text console again (%d colours, desktop had %d)"
          % (len(s5.colours()), len(s1.colours())))

    print("\nDESKTOP: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
finally:
    q.kill()
