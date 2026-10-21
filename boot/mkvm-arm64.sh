#!/usr/bin/env bash
# Build an arm64 aios machine: a minimal Linux whose PID 1 is aios-boot, for
# QEMU on Apple Silicon (HVF) or anywhere (TCG). No root, no Docker, no apt:
#
#   boot/mkvm-arm64.sh [out-dir]          (default build/vm-arm64)
#
# Out: <out>/vmlinuz      Ubuntu 24.04 arm64 generic kernel 6.8 (virtio-blk/pci/net,
#                         ext4, PL011 and Landlock built in, so no initrd: aios-boot is
#                         the first process; 9p and the rest are modules on the disk)
#      <out>/rootfs.img   ext4 (label aios-vm): aios + aios-boot (cross-compiled for
#                         aarch64 Linux), node 22, git, python3, gcc/g++/make/cmake,
#                         bubblewrap, busybox, ca-certificates
#      <out>/packages.lock, kernel.lock   every package taken: name version sha256
#      <out>/SHA256SUMS
#      <out>/aios-vm-arm64.tar.gz   all of the above in one file, to copy to the Mac
#
# Reproducible: packages resolve against Ubuntu's frozen noble release pocket
# (ports.ubuntu.com), with InRelease checked by gpgv against the Ubuntu archive
# keyring and every file by SHA256 (boot/vm-debs.py); node is pinned with its
# published SHA256. Owned by root inside the image via fakeroot.
#
# The image is generic: /etc/aios/rc.d/50-hostboot (run by aios-boot once the
# host is up) mounts the 9p share tagged "hostboot", if the VM was given one,
# and runs boot.sh from it. That is how converge's scripts/aios-vm.sh starts its
# platform inside, without anything converge-specific baked in here.
#
# Host tools (Linux): aarch64-linux-gnu-g++, cmake, dpkg-deb, mke2fs (1.47: -d),
# fakeroot, gpgv + ubuntu-keyring, depmod, curl, xz, python3.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${1:-$HERE/build/vm-arm64}" && cd "${1:-$HERE/build/vm-arm64}" && pwd)
KVER=6.8.0-31-generic
NODE_VERSION=22.23.3
NODE_SHA256=a44aeb94849a299b22df10b9e622ec2f605c2183501bc40590705131de7c740f
PACKAGES="bash coreutils dash util-linux mount findutils grep sed gawk tar gzip xz-utils zstd bzip2 diffutils
  procps iproute2 iputils-ping busybox-static kmod e2fsprogs ca-certificates git openssh-client python3 python3-venv
  gcc g++ make cmake bubblewrap curl less base-files base-passwd libc-bin tzdata file patch"
CACHE=$OUT/cache; STAGE=$OUT/stage; KTREE=$OUT/ktree
mkdir -p "$CACHE"
for t in aarch64-linux-gnu-g++ cmake dpkg-deb mke2fs fakeroot gpgv depmod curl xz python3; do
  command -v "$t" >/dev/null || { echo "missing host tool: $t" >&2; exit 1; }
done

echo "== aios for aarch64 Linux (cross-compiled)"
AB=$OUT/aios-build
cat > "$OUT/aarch64-linux.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
# No SDL2 (headless, and the host's x86 pkg-config would otherwise leak in); no arm64 zlib in the
# cross sysroot either, so HTTP gzip uses aios's own encoder (src/net/gzip.cpp).
PKG_CONFIG_LIBDIR=/nonexistent cmake -S "$HERE" -B "$AB" -DCMAKE_TOOLCHAIN_FILE="$OUT/aarch64-linux.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAIOS_ENABLE_SDL2=OFF > "$OUT/aios-cmake.log"
cmake --build "$AB" -j"$(nproc)" --target aios-boot aios-cli > "$OUT/aios-build.log" 2>&1 \
  || { tail -30 "$OUT/aios-build.log"; exit 1; }

echo "== kernel $KVER and packages (Ubuntu noble arm64, verified)"
python3 "$HERE/boot/vm-debs.py" "$CACHE" "$OUT/kernel.lock" "linux-image-$KVER" "linux-modules-$KVER" "linux-modules-extra-$KVER" > "$OUT/kernel-debs.txt"
python3 "$HERE/boot/vm-debs.py" "$CACHE" "$OUT/packages.lock" $PACKAGES > "$OUT/debs.txt"
NODE_TAR=$CACHE/node-v$NODE_VERSION-linux-arm64.tar.xz
[ -f "$NODE_TAR" ] || curl -fsSLo "$NODE_TAR" "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-arm64.tar.xz"
echo "$NODE_SHA256  $NODE_TAR" | sha256sum -c --quiet - || { rm -f "$NODE_TAR"; echo "node checksum mismatch" >&2; exit 1; }

rm -rf "$KTREE"; mkdir -p "$KTREE"
grep -E "/linux-(image|modules)-[0-9]" "$OUT/kernel-debs.txt" | while read -r d; do dpkg-deb -x "$d" "$KTREE"; done
# From linux-modules-extra only what a QEMU virt guest needs and the base set lacks: the PL031 real-time clock.
rm -rf "$OUT/kextra"; mkdir -p "$OUT/kextra"
dpkg-deb -x "$(grep -E "/linux-modules-extra-" "$OUT/kernel-debs.txt")" "$OUT/kextra"
for m in drivers/rtc/rtc-pl031.ko.zst; do
  mkdir -p "$KTREE/lib/modules/$KVER/kernel/$(dirname "$m")"; cp "$OUT/kextra/lib/modules/$KVER/kernel/$m" "$KTREE/lib/modules/$KVER/kernel/$m"
done
rm -rf "$OUT/kextra"
cp "$KTREE/boot/vmlinuz-$KVER" "$OUT/vmlinuz"

# Everything below runs in one fakeroot session so the image's files belong to root.
rm -rf "$STAGE"; mkdir -p "$STAGE"
fakeroot -- bash -euo pipefail -s "$OUT" "$KVER" "$NODE_TAR" "$HERE" <<'ROOTFS'
OUT=$1; KVER=$2; NODE_TAR=$3; HERE=$4; STAGE=$OUT/stage; KTREE=$OUT/ktree
X=$OUT/extract; rm -rf "$X"; mkdir -p "$X"
while read -r d; do dpkg-deb -x "$d" "$X"; done < "$OUT/debs.txt"
# merged /usr, as Ubuntu lays it out: /bin /sbin /lib → usr/
mkdir -p "$STAGE/usr"
for d in bin sbin lib; do
  mkdir -p "$STAGE/usr/$d"
  if [ -d "$X/$d" ] && [ ! -L "$X/$d" ]; then cp -a "$X/$d/." "$STAGE/usr/$d/"; rm -rf "${X:?}/$d"; fi
  rm -f "$X/$d"; ln -s "usr/$d" "$STAGE/$d"
done
rm -f "$X/lib64"; [ -d "$X/usr/lib64" ] || mkdir -p "$X/usr/lib64"
cp -a "$X/." "$STAGE/"; rm -rf "$X"
ln -sfn usr/lib64 "$STAGE/lib64"
cd "$STAGE"
mkdir -p proc sys dev run tmp root home mnt media var/tmp var/log etc/aios/rc.d aios/bin aios/root aios/mnt
chmod 1777 tmp var/tmp; chmod 700 root
# What maintainer scripts would have done.
cp usr/share/base-passwd/passwd.master etc/passwd; cp usr/share/base-passwd/group.master etc/group
awk -F: '{print $1":*:19000:0:99999:7:::"}' etc/passwd > etc/shadow; chmod 640 etc/shadow
sed -i 's|^root:\*:|root::|' etc/shadow
[ -e usr/bin/sh ] || ln -s dash usr/bin/sh
ln -sf gawk usr/bin/awk; ln -sf gcc usr/bin/cc; ln -sf g++ usr/bin/c++; ln -sf cpp-13 usr/bin/cpp 2>/dev/null || true
ln -sf python3 usr/bin/python
ln -sf busybox usr/bin/udhcpc 2>/dev/null || true
mkdir -p etc/ssl/certs
cat usr/share/ca-certificates/*/*.crt > etc/ssl/certs/ca-certificates.crt
ln -sf /usr/share/zoneinfo/Etc/UTC etc/localtime; echo Etc/UTC > etc/timezone
echo aios-vm > etc/hostname
printf '127.0.0.1 localhost aios-vm\n::1 localhost\n' > etc/hosts
echo 'nameserver 10.0.2.3' > etc/resolv.conf
mkdir -p etc/ld.so.conf.d
[ -f etc/ld.so.conf.d/aarch64-linux-gnu.conf ] || printf '/usr/local/lib/aarch64-linux-gnu\n/lib/aarch64-linux-gnu\n/usr/lib/aarch64-linux-gnu\n' > etc/ld.so.conf.d/aarch64-linux-gnu.conf
printf '[safe]\n\tdirectory = *\n' > etc/gitconfig
cat > etc/profile.d/aios-vm.sh <<'SH'
export PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/aios/bin
SH
# node 22: the official linux-arm64 build
mkdir -p usr/local; tar -xJf "$NODE_TAR" -C usr/local --strip-components=1 --exclude='*/README.md' --exclude='*/CHANGELOG.md' --exclude='*/LICENSE'
# kernel modules (9p, overlay, ...) for kmod's modprobe
mkdir -p usr/lib/modules; cp -a "$KTREE/lib/modules/$KVER" usr/lib/modules/
depmod -b "$STAGE" "$KVER"
# aios: PID 1 (aios-boot) and the command surface it keeps on the console
cp "$OUT/aios-build/aios" "$OUT/aios-build/aios-boot" aios/bin/
aarch64-linux-gnu-strip aios/bin/aios aios/bin/aios-boot
cat > etc/udhcpc.sh <<'SH'
#!/bin/sh
# busybox udhcpc hook (aios-boot starts udhcpc on every interface): apply the lease
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
chmod 755 etc/udhcpc.sh
cat > etc/aios/rc.d/50-hostboot <<'SH'
#!/bin/sh
# aios-boot runs this once the host is up. If the VM was given a 9p share tagged
# "hostboot", mount it read-only and hand over to its boot.sh (a platform the
# machine exists to serve). Nothing else is started here.
export PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/aios/bin
[ -s /etc/ld.so.cache ] || ldconfig
# The clock: QEMU's virt board has a PL031 RTC (a module in this kernel); without it the guest lives in 1970 and
# every TLS certificate is "not yet valid".
modprobe -a rtc-pl031 9p 9pnet_virtio overlay 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -e /dev/rtc0 ] && break; sleep 0.2; done
busybox hwclock -s -u 2>/dev/null || echo "[hostboot] no RTC: the clock is wrong"
grep -qw hostboot /sys/bus/virtio/drivers/9pnet_virtio/*/mount_tag 2>/dev/null || exit 0
mkdir -p /run/hostboot
mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=262144 hostboot /run/hostboot || { echo "[hostboot] mount failed"; exit 1; }
[ -x /run/hostboot/boot.sh ] || [ -f /run/hostboot/boot.sh ] || { echo "[hostboot] no boot.sh in the share"; exit 0; }
exec sh /run/hostboot/boot.sh
SH
chmod 755 etc/aios/rc.d/50-hostboot
rm -f etc/ld.so.cache   # the guest builds its own at boot (50-hostboot); a cross ldconfig cannot
chown -R 0:0 "$STAGE"   # fakeroot: recorded, then read back by mke2fs -d
rm -rf usr/share/doc usr/share/man usr/share/info usr/share/locale usr/share/lintian var/cache/* var/lib/apt
# ext4 with 1.5 GiB free on top of the content (the VM mounts it as an ephemeral snapshot)
SIZE_MB=$(( $(du -sm "$STAGE" | cut -f1) + 1536 ))
rm -f "$OUT/rootfs.img"
mke2fs -q -t ext4 -L aios-vm -E root_owner=0:0 -d "$STAGE" "$OUT/rootfs.img" "${SIZE_MB}M"
ROOTFS

(cd "$OUT" && sha256sum vmlinuz rootfs.img packages.lock kernel.lock > SHA256SUMS)
# One file to carry to another machine (a Mac: converge's `scripts/aios-vm.sh image <this file>`); sparse-aware.
(cd "$OUT" && tar -cSf - vmlinuz rootfs.img packages.lock kernel.lock SHA256SUMS | gzip -6 > aios-vm-arm64.tar.gz)
echo "kernel  $OUT/vmlinuz ($(du -h "$OUT/vmlinuz" | cut -f1))"
echo "rootfs  $OUT/rootfs.img ($(du -h --apparent-size "$OUT/rootfs.img" | cut -f1) apparent, $(du -h "$OUT/rootfs.img" | cut -f1) on disk)"
echo "bundle  $OUT/aios-vm-arm64.tar.gz ($(du -h "$OUT/aios-vm-arm64.tar.gz" | cut -f1))"
echo "packages $(grep -vc '^#' "$OUT/packages.lock") (packages.lock), node $NODE_VERSION, aios $(git -C "$HERE" rev-parse --short HEAD)"
