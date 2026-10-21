#!/usr/bin/env bash
# Wait for metal/build.sh to finish linking, then boot the bare-metal kernel's
# acceptance ISO in QEMU and record whether it passed.
#
# It used to hand the ELF to QEMU's -kernel. QEMU's loader speaks Multiboot 1
# and PVH, not the Multiboot2 this kernel carries, so every run ended
# "Error loading uncompressed kernel without PVH ELF Note" — and the script
# still exited 0. The acceptance ISO (GRUB → multiboot2 … accept) is what
# metal/build.sh --run boots; this boots the same thing.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); out=$here/build/metal
# Whatever qemu is on PATH (Linux, MacPorts), else Homebrew's default prefix.
QEMU=${QEMU:-$(command -v qemu-system-x86_64 || echo /opt/homebrew/bin/qemu-system-x86_64)}
[ -x "$QEMU" ] || { echo "qemu-system-x86_64 not found (Linux: apt install qemu-system-x86; macOS: brew install qemu)" >&2; exit 1; }
log=$out/prove-baremetal.log

# 1. wait for the build to finish (up to 15 min)
for _ in $(seq 1 900); do
  pgrep -f "metal/build[.]sh" >/dev/null || break
  sleep 1
done
if pgrep -f "metal/build[.]sh" >/dev/null; then echo "SETTLE: build never finished" >> "$log"; exit 1; fi

elf=$out/aios-metal.elf
iso=$out/aios-metal-accept.iso
if [ ! -f "$elf" ]; then echo "SETTLE: FAIL no elf at $elf" >> "$log"; exit 1; fi
if [ ! -f "$iso" ] || [ "$iso" -ot "$elf" ]; then echo "SETTLE: FAIL $iso is missing or older than the kernel - rerun metal/build.sh" >> "$log"; exit 1; fi
# The ELF must be newer than the newest source file, or we are booting stale code.
newer=$(find "$here/src" "$here/metal" -name '*.cpp' -o -name '*.S' -o -name '*.hpp' | xargs ls -t 2>/dev/null | head -1)
if [ -n "$newer" ] && [ "$elf" -ot "$newer" ]; then
  echo "SETTLE: FAIL elf is older than $newer - refusing to boot stale kernel" >> "$log"; exit 1
fi
echo "=== ELF $elf ($(wc -c < "$elf" | tr -d ' ') bytes), fresh ===" >> "$log"   # wc, not BSD stat -f / GNU stat -c

# 2. boot it. isa-debug-exit turns the kernel's port-0xf4 write into a QEMU exit.
rm -f "$out/qemu-prove.log"
# timeout, not a `pkill -f qemu…aios-metal` watchdog: that one killed every
# other aios-metal QEMU on the machine too.
# A UEFI-only ISO (no grub i386-pc modules when it was built) needs OVMF.
FW=()
if [ ! -d /usr/lib/grub/i386-pc ]; then
  for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE_4M.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd; do [ -f "$f" ] && FW=(-bios "$f") && break; done
fi
timeout 90 "$QEMU" -cpu max "${FW[@]}" -smp 4 -m 512 -nographic -monitor none -no-reboot \
  -cdrom "$iso" -boot d \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  < /dev/null > "$out/qemu-prove.log" 2>&1
rc=$?
echo "=== qemu exit $rc ===" >> "$log"
sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$out/qemu-prove.log" >> "$log"
if grep -q "ACCEPTANCE: PASS" "$out/qemu-prove.log"; then
  echo "SETTLE: PASS" >> "$log"; echo "SETTLE: PASS"
else
  echo "SETTLE: FAIL" >> "$log"; echo "SETTLE: FAIL (see $log)"; exit 1
fi
