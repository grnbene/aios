#!/usr/bin/env bash
# A deployable bare-metal disk image: MBR, partition 1 = FAT (type 0xEF) with
# GRUB for BIOS and UEFI plus the kernel, partition 2 = the AOS volume (type
# 0x7F, formatted by the kernel on first boot). Boots on BIOS or UEFI to the
# command surface with persistence. Convert with qemu-img for other hypervisors.
#   metal/mkdisk.sh [size-MiB]     → build/metal/aios-metal-disk.img
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); out=$here/build/metal; size=${1:-256}
[ -f "$out/aios-metal.elf" ] || { echo "build the kernel first (metal/build.sh)"; exit 1; }
img=$out/aios-metal-disk.img; work=$out/disk; rm -rf "$work"; mkdir -p "$work"
boot_first=2048; boot_size=$((64 * 2048))                       # 64 MiB boot partition at 1 MiB
vol_first=$((boot_first + boot_size)); vol_size=$((size * 2048 - vol_first))
# --- GRUB images. The BIOS half needs grub's i386-pc modules; without them the
# image is UEFI-only (still a complete, bootable AOS disk on UEFI firmware).
if [ -d /usr/lib/grub/i386-pc ]; then BIOS=1; else BIOS=0; echo "note: grub i386-pc modules absent — building a UEFI-only disk image"; fi
[ "$BIOS" = 1 ] && grub-mkimage -O i386-pc -o "$work/core.img" -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos fat multiboot2 normal echo serial terminal configfile all_video
grub-mkimage -O x86_64-efi -o "$work/BOOTX64.EFI" -p '(hd0,msdos1)/boot/grub' part_msdos fat multiboot2 normal echo serial terminal configfile efi_gop efi_uga all_video
cat > "$work/grub.cfg" <<CFG
serial --unit=0 --speed=115200
insmod all_video
terminal_input serial console
terminal_output serial console
set default=0
set timeout=2
menuentry "AIOS — bare metal (persistent volume on this disk)" { multiboot2 /boot/aios-metal.elf; boot }
menuentry "AIOS — bare metal, acceptance sequence" { multiboot2 /boot/aios-metal.elf accept; boot }
CFG
# --- boot partition (FAT16)
mkfs.fat -C -F 16 -n AIOSBOOT "$work/boot.img" $((boot_size / 2)) >/dev/null
mmd -i "$work/boot.img" ::boot ::boot/grub ::EFI ::EFI/BOOT
mcopy -i "$work/boot.img" "$out/aios-metal.elf" ::boot/aios-metal.elf
mcopy -i "$work/boot.img" "$work/grub.cfg" ::boot/grub/grub.cfg
mcopy -i "$work/boot.img" "$work/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
# --- assemble: MBR (grub boot.img + partition table), core.img at LBA 1, partitions
rm -f "$img"; truncate -s $((size * 1024 * 1024)) "$img"
python3 - "$img" "$work/core.img" $boot_first $boot_size $vol_first $vol_size "$BIOS" <<'PY'
import struct, sys, os
img, core, bf, bs, vf, vs, bios = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:7]), sys.argv[7] == "1"
boot = bytearray(open('/usr/lib/grub/i386-pc/boot.img', 'rb').read()) if bios else bytearray(512)
if bios:
    # GRUB boot.img: kernel sector LBA at 0x5C (8 bytes); boot drive 0x64 = 0xFF (use BIOS drive)
    struct.pack_into('<Q', boot, 0x5C, 1); boot[0x64] = 0xFF
def entry(status, typ, first, count):
    return bytes([status, 0xFE, 0xFF, 0xFF, typ, 0xFE, 0xFF, 0xFF]) + struct.pack('<II', first, count)
table = entry(0x80, 0xEF, bf, bs) + entry(0x00, 0x7F, vf, vs) + bytes(32)
boot[0x1B8:0x1BC] = b'AIOS'; boot[0x1BE:0x1FE] = table; boot[0x1FE:0x200] = b'\x55\xAA'
with open(img, 'r+b') as f:
    f.seek(0); f.write(boot)
    if bios:
        f.seek(512); f.write(open(core, 'rb').read())   # embedding area LBA 1..2047
PY
dd if="$work/boot.img" of="$img" bs=512 seek=$boot_first conv=notrunc status=none
echo "disk image: $img ($size MiB; boot 64 MiB FAT + AOS volume $((vol_size / 2048)) MiB) — $([ "$BIOS" = 1 ] && echo 'BIOS+UEFI' || echo 'UEFI only')"
