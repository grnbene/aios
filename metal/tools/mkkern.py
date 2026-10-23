#!/usr/bin/env python3
"""Pull the kerning pairs out of the faces the atlas is cut from.

The atlas (mkfont.py) carries each glyph's coverage and advance, and nothing
about pairs: "AV", "To", "y." were set at their plain advances, which is the
loose, gappy look text has next to a browser's. DejaVu Sans and Serif carry a
TrueType `kern` table (the same pairs their GPOS has), so the pairs are read
here, at build time, from the very files the atlas was cut from, and shipped as
sorted tables the renderer binary-searches. Mono has none, as a monospace face
should not.

Values stay in font units (DejaVu: 2048 to the em); the renderer scales them by
the size it draws at, so one table serves every cut.

    python3 metal/tools/mkkern.py [--check]

Reads the TTFs from AIOS_DEJAVU_DIR (default /usr/share/fonts/truetype/dejavu,
as mkfont.py does) and writes src/userland/font_kerning.cpp. --check
regenerates and diffs, changing nothing. Only the standard library: the tables
are parsed here directly.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEJAVU = os.environ.get("AIOS_DEJAVU_DIR", "/usr/share/fonts/truetype/dejavu")
OUT = os.path.join(ROOT, "src", "userland", "font_kerning.cpp")

# The families the renderer picks between (web_type.cpp's choose_face) and the
# file each is cut from. Mono is absent on purpose.
FAMILIES = [
    ("SANS", "DejaVuSans.ttf"),
    ("SANS_BOLD", "DejaVuSans-Bold.ttf"),
    ("SANS_ITALIC", "DejaVuSans-Oblique.ttf"),
    ("SANS_BOLD_ITALIC", "DejaVuSans-BoldOblique.ttf"),
    ("SERIF", "DejaVuSerif.ttf"),
    ("SERIF_BOLD", "DejaVuSerif-Bold.ttf"),
    ("SERIF_ITALIC", "DejaVuSerif-Italic.ttf"),
    ("SERIF_BOLD_ITALIC", "DejaVuSerif-BoldItalic.ttf"),
]


def wanted(cp):
    """Code points worth a pair: Latin (with its accents) and the punctuation
    that sits against letters -- quotes, dashes, the ellipsis."""
    return (0x20 <= cp <= 0x24F) or (0x2010 <= cp <= 0x2027)


def tables(data):
    n = struct.unpack(">H", data[4:6])[0]
    out = {}
    for i in range(n):
        tag, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i: 28 + 16 * i])
        out[tag.decode("latin-1")] = (off, length)
    return out


def units_per_em(data, t):
    off, _ = t["head"]
    return struct.unpack(">H", data[off + 18: off + 20])[0]


def glyph_to_cps(data, t):
    """gid -> [code points], from the (3,1) or (0,3) format 4 cmap."""
    off, _ = t["cmap"]
    n = struct.unpack(">H", data[off + 2: off + 4])[0]
    sub = None
    for i in range(n):
        pid, eid, so = struct.unpack(">HHI", data[off + 4 + 8 * i: off + 12 + 8 * i])
        fmt = struct.unpack(">H", data[off + so: off + so + 2])[0]
        if fmt == 4 and ((pid == 3 and eid == 1) or pid == 0):
            sub = off + so
            break
    if sub is None:
        raise SystemExit("no format 4 cmap")
    segx2 = struct.unpack(">H", data[sub + 6: sub + 8])[0]
    seg = segx2 // 2
    ends = sub + 14
    starts = ends + segx2 + 2
    deltas = starts + segx2
    ranges = deltas + segx2
    rev = {}
    for s in range(seg):
        end = struct.unpack(">H", data[ends + 2 * s: ends + 2 * s + 2])[0]
        start = struct.unpack(">H", data[starts + 2 * s: starts + 2 * s + 2])[0]
        delta = struct.unpack(">h", data[deltas + 2 * s: deltas + 2 * s + 2])[0]
        roff = struct.unpack(">H", data[ranges + 2 * s: ranges + 2 * s + 2])[0]
        for cp in range(start, end + 1):
            if cp == 0xFFFF or not wanted(cp):
                continue
            if roff == 0:
                gid = (cp + delta) & 0xFFFF
            else:
                at = ranges + 2 * s + roff + 2 * (cp - start)
                gid = struct.unpack(">H", data[at: at + 2])[0]
                if gid:
                    gid = (gid + delta) & 0xFFFF
            if gid:
                rev.setdefault(gid, []).append(cp)
    return rev


def kern_pairs(data, t):
    """The horizontal format 0 subtables of a version 0 `kern` table: {(lgid, rgid): value}."""
    if "kern" not in t:
        return {}
    off, _ = t["kern"]
    version, ntab = struct.unpack(">HH", data[off: off + 4])
    if version != 0:
        raise SystemExit("kern table version %d is not handled" % version)
    pairs = {}
    at = off + 4
    for _ in range(ntab):
        _, length, coverage = struct.unpack(">HHH", data[at: at + 6])
        fmt = coverage >> 8
        horizontal = coverage & 1
        minimum = coverage & 2
        cross = coverage & 4
        if fmt == 0 and horizontal and not minimum and not cross:
            npairs = struct.unpack(">H", data[at + 6: at + 8])[0]
            p = at + 14
            for _ in range(npairs):
                l, r, v = struct.unpack(">HHh", data[p: p + 6])
                pairs[(l, r)] = pairs.get((l, r), 0) + v
                p += 6
        at += length
    return pairs


def family(path):
    data = open(path, "rb").read()
    t = tables(data)
    upem = units_per_em(data, t)
    rev = glyph_to_cps(data, t)
    out = {}
    for (l, r), v in kern_pairs(data, t).items():
        if v == 0 or l not in rev or r not in rev:
            continue
        for a in rev[l]:
            for b in rev[r]:
                out[(a, b)] = v
    return upem, sorted(out.items())


def render():
    lines = [
        "// GENERATED by metal/tools/mkkern.py from the DejaVu faces the atlas is cut from -- do not edit by hand.",
        "//",
        "// Kerning pairs, in font units, sorted by (left << 16 | right) for a binary search (font_kerning.hpp).",
        '#include "aios/userland/font_kerning.hpp"',
        "",
        "namespace aios::userland::fonts {",
        "namespace {",
        "",
    ]
    entries = []
    for name, fname in FAMILIES:
        upem, pairs = family(os.path.join(DEJAVU, fname))
        lines.append("// %s: %d pairs, %d units to the em" % (fname, len(pairs), upem))
        lines.append("const KernPair k%s[] = {" % name.title().replace("_", ""))
        row = []
        for (a, b), v in pairs:
            row.append("{0x%08Xu, %d}" % ((a << 16) | b, v))
            if len(row) == 6:
                lines.append("    " + ", ".join(row) + ",")
                row = []
        if row:
            lines.append("    " + ", ".join(row) + ",")
        lines.append("};")
        lines.append("")
        entries.append((name, upem, len(pairs)))
    lines.append("}  // namespace")
    lines.append("")
    lines.append("const KernTable kKern[] = {")
    for name, upem, n in entries:
        lines.append("    {k%s, %d, %d},   // KernFamily::%s" % (name.title().replace("_", ""), n, upem, name))
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace aios::userland::fonts")
    return "\n".join(lines) + "\n"


def main():
    text = render()
    if "--check" in sys.argv:
        have = open(OUT).read() if os.path.exists(OUT) else ""
        if have != text:
            print("font_kerning.cpp is out of date; run metal/tools/mkkern.py")
            return 1
        print("font_kerning.cpp is current")
        return 0
    with open(OUT, "w") as f:
        f.write(text)
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
