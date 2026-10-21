#!/usr/bin/env bash
# Flash the freshly built AIOS AArch64 bare-metal kernel to Raspberry Pi 3 boot media.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
kernel_bin="$here/build/metal-arm64/aios-metal-arm64.bin"
kernel_elf="$here/build/metal-arm64/aios-metal-arm64.elf"

if [ ! -f "$kernel_bin" ]; then
  echo "Kernel binary not found at $kernel_bin — building first..."
  "$here/metal/build-arm64.sh"
fi

echo "Looking for Raspberry Pi boot partition..."

# Check common mount points or search for vfat partitions on removable drives
mount_point=""
for p in /media/arch/boot /media/arch/bootfs /run/media/arch/boot /run/media/arch/bootfs /mnt/rpi_boot; do
  if [ -d "$p" ] && mountpoint -q "$p" 2>/dev/null; then
    mount_point="$p"
    break
  fi
done

# If not mounted, look for /dev/sdc1 or other removable vfat partition
if [ -z "$mount_point" ]; then
  # Probe every plausible removable partition, not a hardcoded three. The
  # reader does not get a stable name: it has come up as sdc, sde and sdf on
  # this machine alone, and when it landed outside the old sdc/sdd/sde list
  # this script exited without flashing anything - which looks exactly like
  # a flash that worked but changed nothing.
  for dev in /dev/sd[b-z]1; do
    [ -b "$dev" ] || continue

    # Only consider vfat - a Pi boot partition is always FAT, and this keeps
    # the wider probe above from ever touching an ext4 data disk.
    fstype=$(lsblk -no FSTYPE "$dev" 2>/dev/null || echo "")
    [ "$fstype" = "vfat" ] || continue

    echo "Trying $dev (vfat), mounting to /tmp/rpi_boot..."
    mkdir -p /tmp/rpi_boot
    sudo mount "$dev" /tmp/rpi_boot 2>/dev/null || continue
    if ! mountpoint -q /tmp/rpi_boot 2>/dev/null; then
      continue
    fi

    # Now PROVE it is a Raspberry Pi boot partition before writing a byte to
    # it. Widening the device probe without this check would be a good way
    # to overwrite somebody's USB stick.
    if [ -f /tmp/rpi_boot/bootcode.bin ] || [ -f /tmp/rpi_boot/start.elf ] || \
       [ -f /tmp/rpi_boot/config.txt ]; then
      echo "  -> looks like a Raspberry Pi boot partition"
      mount_point="/tmp/rpi_boot"
      break
    fi

    echo "  -> not a Pi boot partition (no bootcode.bin/start.elf/config.txt), skipping"
    sudo umount /tmp/rpi_boot 2>/dev/null || true
  done
fi

if [ -z "$mount_point" ]; then
  echo "Boot partition not yet mounted. Current block devices:"
  lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT /dev/sd* 2>/dev/null || true
  echo "Please ensure the microSD card is fully seated in the USB reader."
  exit 2
fi

echo "Flashing AIOS to $mount_point..."

# Backup original kernel if not backed up
if [ -f "$mount_point/kernel8.img" ] && [ ! -f "$mount_point/kernel8-linux.img" ]; then
  echo "Backing up original kernel8.img -> kernel8-linux.img..."
  sudo cp -p "$mount_point/kernel8.img" "$mount_point/kernel8-linux.img"
fi

# Restore the GPU firmware blobs if they are missing. This script used to
# assume bootcode.bin/start.elf/fixup.dat were already on the card (true for
# a factory Raspberry Pi OS card, never true once strip-sd-to-netboot.sh has
# run) and never copied them - so a flash onto a stripped card wrote a valid
# kernel8.img with nothing on the card able to ever load it: no start.elf,
# no GPU firmware, no HDMI init, no config.txt read, ARM core never started.
# Total silence on both video and network, indistinguishable from a kernel
# bug and NOT explained by which kernel build got flashed - which is exactly
# what happened here. Vendored copies live in metal/rpi-firmware/.
fw_dir="$here/metal/rpi-firmware"
for f in bootcode.bin start.elf fixup.dat; do
  if [ ! -f "$mount_point/$f" ]; then
    if [ ! -f "$fw_dir/$f" ]; then
      echo "FATAL: $mount_point/$f missing and no vendored copy at $fw_dir/$f - card cannot boot without it."
      exit 3
    fi
    echo "Restoring missing GPU firmware file: $f"
    sudo cp -v "$fw_dir/$f" "$mount_point/$f"
  fi
done

# Copy AIOS kernel images
sudo cp -v "$kernel_bin" "$mount_point/aios-metal-arm64.bin"
sudo cp -v "$kernel_bin" "$mount_point/kernel8.img"
sudo cp -v "$kernel_elf" "$mount_point/aios-metal-arm64.elf"

# Initialize default wifi.cfg if not already present on card
if [ ! -f "$mount_point/wifi.cfg" ]; then
  sudo cp -v "$here/metal/arm64/wifi.cfg" "$mount_point/wifi.cfg"
fi

# Configure Raspberry Pi config.txt for 64-bit AArch64 bare-metal boot

cat << 'EOF' | sudo tee "$mount_point/config.txt" >/dev/null
# Raspberry Pi 3 Bare-Metal AIOS Configuration
arm_64bit=1
kernel=kernel8.img

# One-time OTP fuse enable for network/USB boot fallback (original Pi 3B,
# BCM2837, needs OTP bit 29 set - default OTP is 0x1020000a, this asks the
# stock GPU firmware to burn it to 0x3020000a on THIS boot). Burning is
# permanent and harmless to leave here after that - it does not change
# normal SD-card boot behavior at all, it only adds netboot as a fallback
# path. See metal/serve-netboot.sh and aios-metal-arm64-netboot memory
# for the other half of this (TFTP server + stripping the SD to
# bootcode.bin-only so it actually falls through to the network).
program_usb_boot_mode=1

# Enable UART console on GPIO 14 (TX) & 15 (RX) at 115200 baud
enable_uart=1
uart_2ndstage=1
core_freq=250

# HDMI Display Framebuffer Configuration
hdmi_force_hotplug=1
disable_overscan=1
# No framebuffer_width/height: the kernel asks the firmware for the
# monitor's native mode and renders at it (a fixed 1280x720 was upscaled
# by the GPU on 1080p screens and made all text soft).
framebuffer_depth=32
framebuffer_ignore_alpha=1

# Disable kernel commandline requirement for bare-metal
disable_commandline_tags=1
EOF

echo "Committing writes to storage..."
sync

# Verify what actually landed on the card, rather than assuming the copy
# worked. "I flashed it and nothing changed" has cost real hours here; a
# byte-for-byte check turns that into a fact instead of a guess.
src_md5=$(md5sum "$kernel_bin" | awk '{print $1}')
dst_md5=$(md5sum "$mount_point/kernel8.img" 2>/dev/null | awk '{print $1}')

echo
echo "Verifying:"
echo "  source $kernel_bin"
echo "    md5 $src_md5"
echo "  on card $mount_point/kernel8.img"
echo "    md5 ${dst_md5:-<missing>}"

if [ "$src_md5" != "$dst_md5" ]; then
  echo
  echo "MISMATCH - the card does NOT contain the kernel that was just built."
  echo "Do not bother booting it; nothing you changed is on there."
  exit 1
fi

# Surface the build stamp that is compiled into the image, so the string
# printed on the serial console at boot can be matched against this exact
# flash without having to guess.
stamp=$(strings "$kernel_bin" | grep -m1 'AArch64 bare metal \[build' || true)
echo
echo "VERIFIED - card matches the build."
[ -n "$stamp" ] && echo "  boot banner should read: $stamp"
echo
echo "Summary of $mount_point:"
ls -lh "$mount_point"/*.img "$mount_point"/aios-metal-arm64.* "$mount_point"/config.txt 2>/dev/null || true
