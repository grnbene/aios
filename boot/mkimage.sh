#!/usr/bin/env bash
# Assemble installable media: an ISO that boots on BIOS (isolinux) and UEFI
# (systemd-boot) straight into the AIOS as PID 1, carrying the installer.
#
#   boot/mkimage.sh <build-dir> <kernel-tree> [out-dir]
#     <kernel-tree>: a directory holding boot/vmlinuz-* and lib/modules/<ver>
#                    (e.g. an extracted Ubuntu linux-image + linux-modules deb)
# Host tools: cpio xz zstd grub-mkrescue (grub-common grub-pc-bin grub-efi-amd64-bin,
# which pulls xorriso and mtools), plus systemd-boot-efi, syslinux-common,
# extlinux, busybox-static installed.
set -euo pipefail
BUILD=$(cd "$1" && pwd); KTREE=$(cd "$2" && pwd); OUT=${3:-$BUILD/media}
HERE=$(cd "$(dirname "$0")" && pwd)
KVER=$(basename "$(ls -d "$KTREE"/lib/modules/*)")
VMLINUZ=$(ls "$KTREE"/boot/vmlinuz-*)
STAGE=$OUT/initramfs; TREE=$OUT/iso
rm -rf "$OUT"; mkdir -p "$STAGE" "$TREE"
echo "kernel $KVER"

# --- initramfs: the whole system lives here; disks hold only persistent state
cd "$STAGE"
mkdir -p bin lib lib64 lib/x86_64-linux-gnu proc sys dev run tmp media mnt/esp mnt/root aios/bin aios/root aios/mnt etc
cp "$BUILD/aios" "$BUILD/aios-boot" "$BUILD/aios-mind-peer" "$BUILD/mind_reflect.so" "$BUILD/mind_fast_graph.so" aios/bin/
cp /usr/sbin/mkfs.fat /sbin/mke2fs /sbin/e2fsck /usr/bin/extlinux aios/bin/
cp "$BUILD/aios-boot" init   # PID 1
cp /bin/busybox bin/busybox
for a in sh ash ls cat cp mv rm mkdir mount umount modprobe insmod lsmod dmesg ps sync reboot poweroff sleep echo grep vi less mkfs.vfat blkid df free head tail hexdump ip udhcpc ping ifconfig route nc wget; do ln -sf busybox bin/$a; done
# shared libraries for every dynamic binary we ship
for b in aios/bin/aios aios/bin/aios-boot aios/bin/aios-mind-peer aios/bin/mkfs.fat aios/bin/mke2fs aios/bin/e2fsck aios/bin/extlinux aios/bin/mind_reflect.so aios/bin/mind_fast_graph.so; do
  ldd "$b" 2>/dev/null | awk '/=> \//{print $3} /^[[:space:]]*\/(lib|usr)/{print $1}' | while read -r lib; do
    case "$lib" in /lib64/*) cp --update=none "$lib" lib64/ ;; *) cp --update=none "$lib" lib/x86_64-linux-gnu/ ;; esac
  done
done
ls lib64/ld-linux-x86-64.so.2 >/dev/null
# kernel modules the VM may need, decompressed so busybox modprobe can load them
mkdir -p "lib/modules/$KVER"
cp "$KTREE/lib/modules/$KVER/modules.order" "$KTREE/lib/modules/$KVER/modules.builtin" "lib/modules/$KVER/" 2>/dev/null || true
want="e1000 e1000e vmxnet3 hv_netvsc virtio_net ahci libahci isofs nls_iso8859-1 vmw_pvscsi mptspi mptscsih mptbase mptsas mpt3sas scsi_transport_spi scsi_transport_sas raid_class hv_vmbus hv_storvsc hyperv_keyboard serio xhci-pci xhci-pci-renesas xhci-hcd usb-storage uas usbhid hid-generic hid"
for m in $want; do
  f=$(find "$KTREE/lib/modules/$KVER/kernel" -name "$m.ko.zst" | head -1) || true
  [ -n "$f" ] || { echo "  (no module $m — built in or absent)"; continue; }
  rel=${f#"$KTREE/lib/modules/$KVER/"}; rel=${rel%.zst}
  mkdir -p "lib/modules/$KVER/$(dirname "$rel")"; zstd -q -d "$f" -o "lib/modules/$KVER/$rel"
done
busybox depmod -b "$STAGE" "$KVER"
echo "aios" > etc/hostname
cat > etc/udhcpc.sh <<'SH'
#!/bin/sh
# busybox udhcpc hook: apply the lease
case "$1" in
  bound|renew)
    busybox ip addr flush dev "$interface" 2>/dev/null
    busybox ip addr add "$ip/${mask:-24}" dev "$interface"
    [ -n "$router" ] && for r in $router; do busybox ip route add default via "$r" dev "$interface" 2>/dev/null; done
    [ -n "$dns" ] && { : > /etc/resolv.conf; for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done; }
    ;;
  deconfig) busybox ip addr flush dev "$interface" 2>/dev/null ;;
esac
SH
chmod +x etc/udhcpc.sh
find . -print0 | cpio --null -o -H newc --quiet | xz --check=crc32 -9 -T0 > "$OUT/initramfs.img"
echo "initramfs $(du -h "$OUT/initramfs.img" | cut -f1)"

# --- ISO: grub-mkrescue reads ONE copy of kernel+initramfs for both BIOS and UEFI
cd "$TREE"
mkdir -p boot/grub
cp "$VMLINUZ" boot/vmlinuz; cp "$OUT/initramfs.img" boot/initramfs.img
# what the installer lays onto a disk's ESP
cp /usr/lib/systemd/boot/efi/systemd-bootx64.efi boot/
cp /usr/lib/syslinux/mbr/gptmbr.bin boot/; cp /usr/lib/syslinux/modules/bios/ldlinux.c32 boot/
cp "$HERE/media/syslinux-disk.cfg" boot/; cp -r "$HERE/media/loader-disk" boot/loader-disk
cp "$HERE/media/grub.cfg" boot/grub/grub.cfg
cat > README.txt <<TXT
AIOS installable media.
Boot this ISO (BIOS or UEFI). It starts the AIOS command surface directly.
  host disks              list disks
  host install /dev/sda   erase that disk and install AIOS (UEFI + BIOS boot)
  host reboot
TXT
grub-mkrescue -o "$OUT/aios.iso" --volid AIOS --locales= --themes= --fonts= "$TREE" 2>&1 | grep -E "error|warning|ISO image produced" || true
echo "iso $(du -h "$OUT/aios.iso" | cut -f1) → $OUT/aios.iso"
