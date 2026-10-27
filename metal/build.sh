#!/usr/bin/env bash
# Build the unchanged kernel core for bare metal and boot it in QEMU.
#   metal/build.sh [--run]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); out=$here/build/metal; mkdir -p "$out/obj"
CXX=${CXX:-g++}
# Preflight: this kernel is x86-64 bare metal. An arm64-native compiler (Apple clang,
# or brew gcc-15 on Apple Silicon) rejects the x86-only flags with a cryptic
# "unrecognized command-line option '-mno-red-zone'" hundreds of lines into the log.
# Fail here, with the reason, instead.
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "ERROR: CXX=$CXX not found" >&2; exit 1
fi
if ! printf 'int main(void){return 0;}\n' | "$CXX" -x c -c -o /dev/null -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone - >/dev/null 2>&1; then
  echo "ERROR: $CXX cannot target x86-64 bare metal (-mcmodel=kernel -mno-red-zone rejected)." >&2
  echo "       bare metal needs an x86_64 compiler; on Apple Silicon 'g++' is arm64." >&2
  echo "       set CXX to an x86_64 toolchain, e.g.:" >&2
  echo "         CXX=/usr/local/opt/mingw-w64/bin/g++  or  a linux-x86_64 cross gcc" >&2
  echo "       (or build this target on an x86_64 host / in the metal container)" >&2
  exit 1
fi
GCCLIB=$($CXX -print-file-name=libstdc++.a | xargs dirname)
CORE=$(find "$here/src" -name '*.cpp' | grep -vE "src/hal/|src/intelligence/(plugin|external_process|wire_hosted)\.cpp")
FLAGS="-std=c++20 -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-exceptions -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-threadsafe-statics -nostdlib -DAIOS_METAL=1 -I$here/include -Wall -Wno-unused-parameter"
# -mcmodel=kernel expects the image in the top 2 GiB; we link at 1 MiB, so use small non-PIC instead.
FLAGS=${FLAGS/-mcmodel=kernel/-mcmodel=small}
objs=()
# Every cli/*.cpp that commands.cpp dispatches into has to be named here, or the
# bare-metal link fails on a function that plainly exists in the tree. dream,
# organ and face were added over 2026-09-09..10 and this list was not, so the
# metal target had been unbuildable for two days while the hosted one was fine.
CLI="$here/cli/commands.cpp $here/cli/command_docs.cpp $here/cli/embed.cpp $here/cli/out.cpp $here/cli/shell.cpp $here/cli/console.cpp $here/cli/ask.cpp $here/cli/dream.cpp $here/cli/organ.cpp $here/cli/face.cpp $here/cli/grain_cmd.cpp $here/cli/sat_cmd.cpp $here/cli/grain_net.cpp $here/cli/grain_host.cpp $here/cli/model_controller.cpp"
# head closes the pipe as soon as it has its line, which makes xargs die of
# SIGPIPE — and under `set -o pipefail` that killed the whole build before it
# printed anything, intermittently, depending on how fast xargs got there. It is
# a timestamp probe; it is allowed to fail.
newest_hdr=$(find "$here/include" "$here/metal" "$here/cli" -name '*.hpp' -o -name '*.h' | xargs ls -t 2>/dev/null | head -1 || true)
JOBS=${JOBS:-$( (nproc 2>/dev/null) || (sysctl -n hw.ncpu 2>/dev/null) || echo 8)}
CC_PIDS=()
for f in $CORE $here/src/hal/volume.cpp $here/src/hal/fabric.cpp $here/metal/runtime.cpp $here/metal/interrupts.cpp $here/metal/process.cpp $here/metal/hal_metal.cpp $here/metal/smp.cpp $here/metal/virtio_blk.cpp $here/metal/nvme.cpp $here/metal/pci.cpp $here/metal/virtio_net.cpp $here/metal/e1000.cpp $here/metal/virtio_gpu.cpp $here/metal/wire_mind.cpp $here/metal/desktop.cpp $here/metal/canvas.cpp $here/metal/acceptance.cpp $here/metal/main.cpp $CLI; do
  o="$out/obj/$(echo "${f#$here/}" | tr '/' '_').o"; objs+=("$o")
  flags="$FLAGS"
  case "$f" in
    *metal/runtime.cpp) flags="$flags -fno-builtin -fno-tree-loop-distribute-patterns";;
    *cli/*|*metal/main.cpp) flags="${flags/-fno-exceptions/} "; flags="${flags/-fno-asynchronous-unwind-tables/}"; flags="${flags/-fno-unwind-tables/}";;   # the command surface uses exceptions
  esac
  if [ "$o" -nt "$f" ] && [ "$o" -nt "$here/metal/build.sh" ] && [ "$o" -nt "$newest_hdr" ]; then :; else
    # `wait -n` is bash 4.3+; macOS ships 3.2, where it is an invalid option and
    # takes the `|| wait` branch down with it. Throttle on the job count alone.
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
      sleep 0.05
    done
    ( $CXX $flags -c "$f" -o "$o" || { rm -f "$o"; exit 1; } ) &
    CC_PIDS+=($!)
  fi
done
cc_failed=0
for p in "${CC_PIDS[@]}"; do wait "$p" || cc_failed=1; done
if [ "$cc_failed" = 1 ]; then echo "compile failed" >&2; exit 1; fi
$CXX -c "$here/metal/boot.S" -o "$out/obj/boot.o"
$CXX -c "$here/metal/isr.S" -o "$out/obj/isr.o"
# The programs the machine ships with. Real ELF executables, built with no libc
# and with SSE left on, because the kernel now saves and restores that register
# file across a context switch and metal/user/fpu.c is there to prove it.
# and linked where user space lives on this machine (0x8000000000 — past the end
# of the kernel's identity map, so a process cannot even name kernel memory).
# They are embedded as bytes and written into the machine's own filesystem at
# boot, so running one takes the same path as running anything else.
mkdir -p "$out/user"
UFLAGS="-std=gnu11 -O2 -ffreestanding -nostdlib -static -no-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -mcmodel=large -Wall"
{
  echo '.section .rodata'
  for u in hello probe crash priv peek spin fs mem forker execer sig fpu edges; do
    ${CC:-gcc} $UFLAGS -Wl,-e,_start -Wl,-Ttext-segment=0x8000000000 -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
         -o "$out/user/$u" "$here/metal/user/$u.c"
    echo ".balign 16"
    echo ".globl user_${u}_start"; echo "user_${u}_start:"
    echo ".incbin \"$out/user/$u\""
    echo ".globl user_${u}_end"; echo "user_${u}_end:"
  done
  echo '.section .note.GNU-stack,"",@progbits'
} > "$out/obj/user_blobs.S"
$CXX -x assembler-with-cpp -c "$out/obj/user_blobs.S" -o "$out/obj/user_blobs.o"
# AP trampoline: assemble flat, extract raw bytes, wrap as a data object with start/end symbols
$CXX -c "$here/metal/ap_trampoline.S" -o "$out/obj/ap_tramp_full.o"
# Link at base 0 so the symbol relocations (GDT base, far-jump targets) resolve,
# then extract the flat bytes. TRAMP (0x8000) is added inside the source.
ld -m elf_x86_64 -Ttext=0 -e ap_trampoline_start -o "$out/obj/ap_tramp.elf" "$out/obj/ap_tramp_full.o"
objcopy -O binary -j .text "$out/obj/ap_tramp.elf" "$out/obj/ap_tramp.bin"
( echo '.section .rodata'; echo '.globl ap_trampoline_start'; echo '.globl ap_trampoline_end'; echo 'ap_trampoline_start:'; echo -n '.incbin "'; echo -n "$out/obj/ap_tramp.bin"; echo '"'; echo 'ap_trampoline_end:'; echo '.section .note.GNU-stack,"",@progbits' ) > "$out/obj/ap_tramp_blob.S"
$CXX -c "$out/obj/ap_tramp_blob.o.S" -o "$out/obj/ap_tramp.o" 2>/dev/null || $CXX -x assembler-with-cpp -c "$out/obj/ap_tramp_blob.S" -o "$out/obj/ap_tramp.o"
objs+=("$out/obj/ap_tramp.o")
$CXX -static -nostdlib -no-pie -Wl,-T,"$here/metal/link.ld" -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -o "$out/aios-metal.elf" \
  "$out/obj/boot.o" "$out/obj/isr.o" "$out/obj/user_blobs.o" "${objs[@]}" "$GCCLIB/libstdc++.a" "$GCCLIB/libsupc++.a" "$GCCLIB/libgcc_eh.a" "$GCCLIB/libgcc.a"
echo "kernel image: $(du -h "$out/aios-metal.elf" | cut -f1)  $(nm -u "$out/aios-metal.elf" | wc -l) undefined symbols"
rm -rf "$out/iso"; mkdir -p "$out/iso/boot/grub"
cp "$out/aios-metal.elf" "$out/iso/boot/"
mkcfg() {
  default=$1; timeout=$2
  cat <<CFG > "$out/iso/boot/grub/grub.cfg"
insmod serial
insmod all_video
insmod vbe
insmod gfxterm
serial --speed=115200 --unit=0 --word=8 --parity=no --stop=1
terminal_input serial console
terminal_output serial console
# A real graphical mode, so the framebuffer desktop has pixels to composite
# onto: the first size the firmware actually offers wins, and "auto" is the
# last resort (which on a plain BIOS box is VGA text and no desktop). 1080p
# first — the desktop lays itself out from the mode it is given, and the text
# is rasterised from real outlines, so more pixels is simply a better display
# rather than the same picture magnified.
set gfxmode=1920x1080x32,1600x900x32,1440x900x32,1280x1024x32,1280x800x32,1024x768x32,1280x1024,1024x768,800x600,auto
set gfxpayload=keep
set default=$default
set timeout=$timeout
menuentry "AIOS — bare metal, command surface on serial and display" { multiboot2 /boot/aios-metal.elf; boot }
menuentry "AIOS — bare metal, run the acceptance sequence and halt" { multiboot2 /boot/aios-metal.elf accept; boot }
CFG
}
# grub-mkrescue only writes a BIOS boot record when grub's i386-pc modules are
# installed; without them the media is UEFI-only and must be booted with OVMF.
if [ -d /usr/lib/grub/i386-pc ]; then PLATFORMS="BIOS+UEFI"; FW=""; else
  PLATFORMS="UEFI only (install grub-pc-bin for BIOS too)"; FW=""
  for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE_4M.fd; do [ -f "$f" ] && FW="-bios $f" && break; done
fi
GRUB_MKRESCUE=$(command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || echo "grub-mkrescue")
# These were `|| true`, which hid the failure: with no grub-mkrescue installed the
# command printed nothing, the *stale* ISO from a previous run survived, and
# `--run` then booted the old kernel while reporting success. A missing
# ISO is now a hard error.
mkrescue() {
  if ! "$GRUB_MKRESCUE" -o "$1" --locales= --themes= --fonts= "$out/iso" >/dev/null 2>&1; then
    echo "ERROR: grub-mkrescue failed to build $1" >&2
    echo "       install it (macOS: brew install xorriso; Linux: apt install grub-pc-bin grub-efi-amd64-bin xorriso)" >&2
    return 1
  fi
}
mkcfg 0 3; mkrescue "$out/aios-metal.iso"
mkcfg 1 0; mkrescue "$out/aios-metal-accept.iso"
echo "iso: $out/aios-metal.iso  (acceptance variant: aios-metal-accept.iso) — bootable on $PLATFORMS"
if [ "${1:-}" = "--run" ]; then
  set +e
  QEMU=$(command -v qemu-system-x86_64 || echo /opt/homebrew/bin/qemu-system-x86_64)
  if [ ! -x "$QEMU" ]; then echo "ERROR: qemu-system-x86_64 not found (install: brew install qemu)" >&2; exit 1; fi
  # If the acceptance ISO predates the kernel we just linked, the run below would
  # boot the OLD kernel and report a PASS that means nothing. Refuse that.
  if [ ! "$out/aios-metal-accept.iso" -nt "$out/aios-metal.elf" ]; then
    echo "ERROR: $out/aios-metal-accept.iso is older than the freshly linked kernel — refusing to report a stale boot" >&2
    exit 1
  fi
  timeout 60 "$QEMU" -cpu max $FW -smp 4 -m 512 -nographic -monitor none -no-reboot -cdrom "$out/aios-metal-accept.iso" -boot d -device isa-debug-exit,iobase=0xf4,iosize=0x04 < /dev/null > "$out/run.log" 2>&1
  rc=$?
  sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$out/run.log" | grep -vE "^\s*$" | grep -E "^\[metal\]|^kernel|^host|^devices|^accel|^acceptance|^  |^[0-9]+/|^ACCEPTANCE|PANIC" || true
  echo "qemu exit $rc"; [ "$rc" = 33 ] && exit 0 || exit 1
fi


