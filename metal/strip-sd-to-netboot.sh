#!/usr/bin/env bash
# Put the Pi 3's boot SD card into "bootcode.bin-only" mode, so every boot
# fetches start.elf/fixup.dat/config.txt/kernel8.img over TFTP from the
# aios-netboot service on this host - a build is then the whole deploy.
#
#   metal/strip-sd-to-netboot.sh --yes       convert the card
#   metal/strip-sd-to-netboot.sh --restore   put the SD-boot files back
#
# Why this works on an original Pi 3B without the OTP netboot fuse: a
# bootcode.bin on the SD card performs the network boot itself when it finds
# no start.elf beside it. The fuse only matters for booting with NO card.
#
# Nothing is deleted: the SD-boot files move into sdboot-backup/ on the card
# (bootcode.bin only looks at the partition root), so --restore is a move
# back. Kept at the root: bootcode.bin (replaced with the vendored one that
# matches the start.elf served over TFTP), wifi.cfg (the aios kernel reads
# it from the card), and an empty `timeout` file, which makes bootcode.bin
# retry the network after 10 s instead of hanging if the link is slow.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-}
[ "$mode" = "--yes" ] || [ "$mode" = "--restore" ] || { sed -n '2,8p' "$0"; exit 1; }

mount_point=${SD_BOOT_MOUNT:-}   # override for testing against a mounted image
[ -n "$mount_point" ] || while read -r target; do
  if [ -f "$target/bootcode.bin" ]; then mount_point=$target; break; fi
done < <(lsblk -rno RM,FSTYPE,MOUNTPOINT | awk '$1==1 && $2=="vfat" && $3!="" {print $3}')
if [ -z "$mount_point" ]; then
  echo "No mounted removable vfat partition with bootcode.bin found - insert the Pi's SD card." >&2
  exit 2
fi
echo "Pi boot partition: $mount_point"
backup=$mount_point/sdboot-backup

if [ "$mode" = "--restore" ]; then
  [ -d "$backup" ] || { echo "No $backup - card was not converted by this script." >&2; exit 3; }
  for f in "$backup"/*; do [ -e "$f" ] && mv -f "$f" "$mount_point/"; done
  rmdir "$backup"; rm -f "$mount_point/timeout"
  sync; echo "Restored SD boot:"; ls -la "$mount_point"; exit 0
fi

mkdir -p "$backup"
for f in "$mount_point"/*; do
  name=$(basename "$f")
  case "$name" in
    bootcode.bin|wifi.cfg|sdboot-backup|timeout) ;;
    *) mv -f "$f" "$backup/" ;;
  esac
done
cp -f "$mount_point/bootcode.bin" "$backup/bootcode.bin"
cp -f "$here/metal/rpi-firmware/bootcode.bin" "$mount_point/bootcode.bin"
: > "$mount_point/timeout"
sync

if [ -e "$mount_point/start.elf" ] || [ -e "$mount_point/kernel8.img" ]; then
  echo "start.elf or kernel8.img still at the root - card would still SD-boot." >&2
  exit 4
fi
echo "Card is now bootcode.bin-only:"
ls -la "$mount_point"
echo
if systemctl is-active --quiet aios-netboot; then
  echo "aios-netboot is running. Put the card in the Pi and power it on."
else
  echo "WARNING: aios-netboot is not running - start it: sudo systemctl start aios-netboot" >&2
fi
