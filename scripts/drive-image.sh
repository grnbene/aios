#!/bin/sh
# The kernel's image decoders against an independent implementation's pixels.
#
# scripts/web-corpus/photo.jpg is decoded by this machine and compared with
# golden/photo.ref.ppm, which is what libjpeg made of the same bytes. The
# reference is committed, so this needs neither a network nor a Python imaging
# library to run.
#
#   scripts/drive-image.sh <build-dir>
set -eu
BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
CHECK="$BUILD/aios-image-check"
[ -x "$CHECK" ] || { echo "drive.image: no $CHECK" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CHECK" "$CORPUS/photo.jpg" "$WORK/photo.ppm"

# Mean absolute difference per channel, in awk over the raw bytes, so the
# comparison has no dependency of its own. Anything over 1.0 is a decode that
# has gone wrong, not a rounding difference.
python3 - "$WORK/photo.ppm" "$CORPUS/golden/photo.ref.ppm" <<'PY' || exit 1
import sys
def read_ppm(path):
    with open(path,'rb') as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i] != 10: i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace(): j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w*h*3]
aw, ah, a = read_ppm(sys.argv[1])
bw, bh, b = read_ppm(sys.argv[2])
if (aw, ah) != (bw, bh):
    print("drive.image: %dx%d decoded, %dx%d expected" % (aw, ah, bw, bh)); sys.exit(1)
tot = worst = 0
for x, y in zip(a, b):
    d = abs(x - y); tot += d
    if d > worst: worst = d
mean = tot / float(len(a))
print("drive.image: photo.jpg  mean abs diff %.3f  worst %d  (%dx%d)" % (mean, worst, aw, ah))
sys.exit(0 if mean <= 1.0 and worst <= 24 else 1)
PY
