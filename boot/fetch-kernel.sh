#!/usr/bin/env bash
# Fetch a hardware-host kernel for the media without installing anything:
# Ubuntu's generic kernel (virtio, SCSI, SATA, ext4 and FAT built in), extracted
# from its .deb files into <out-dir>/boot and <out-dir>/lib/modules/<ver>.
#   boot/fetch-kernel.sh <out-dir> [linux-image-virtual]
set -euo pipefail
OUT=$(mkdir -p "$1" && cd "$1" && pwd); META=${2:-linux-image-virtual}
cd "$OUT"
apt-get download "$META" >/dev/null
IMG=$(dpkg-deb -f "$META"_*.deb Depends | tr ',' '\n' | grep -o 'linux-image-[0-9][^ ]*' | head -1)
MODS=${IMG/linux-image/linux-modules}
echo "kernel package: $IMG"
apt-get download "$IMG" "$MODS" >/dev/null
for d in "$IMG"_*.deb "$MODS"_*.deb; do dpkg-deb -x "$d" .; done
rm -f ./*.deb
ls boot/vmlinuz-* lib/modules
