#!/usr/bin/env bash
# Build the AArch64 bare-metal kernel and boot it.
#   metal/build-arm64.sh [--run]
#
# The substrate (SRCS below) is FREESTANDING: no libc, no libstdc++, not even
# a memcpy beyond what krt.cpp provides by hand. src/ used to be entirely
# unreachable from here for exactly that reason - but metal/build.sh already
# proved, on x86, that a real libstdc++.a CAN be statically linked into a
# freestanding kernel by supplying the handful of C-ABI primitives it assumes
# (memory/string ops, a heap, printf, libstdc++'s own throw-points redirected
# to metal_panic()). aarch64-linux-gnu-g++ ships a real static libstdc++.a
# too (checked via -print-file-name, not assumed), so the same trick applies
# here: STLSRCS below compiles with ordinary hosted C++ flags (real STL
# headers, RTTI on) instead of SRCS's freestanding ones, and the final link
# adds libstdc++.a/libsupc++.a/libgcc_eh.a alongside libgcc.a. metal/arm64/
# cxx_runtime.cpp + hal_metal_arm64.cpp are the primitives; entity.cpp is the
# first real src/ module proven to link and is the toehold for pulling in
# fabric/peer_discovery/ASQL next.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/metal-arm64
mkdir -p "$out/obj" "$out/user"

# Auto-detect toolchain prefix (prefer aarch64-elf-, fall back to aarch64-linux-gnu-)
PREFIX=aarch64-elf-
if ! command -v "${PREFIX}gcc" >/dev/null 2>&1 && command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  PREFIX=aarch64-linux-gnu-
fi

CXX=${CXX_ARM64:-${PREFIX}g++}
CC=${CC_ARM64:-${PREFIX}gcc}
LD=${LD_ARM64:-${PREFIX}ld}
OBJCOPY=${OBJCOPY_ARM64:-${PREFIX}objcopy}
NM=${NM_ARM64:-${PREFIX}nm}

for tool in "$CXX" "$CC" "$LD" "$OBJCOPY"; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing $tool — install an aarch64 cross toolchain (e.g. gcc-aarch64-linux-gnu g++-aarch64-linux-gnu)" >&2
    exit 1
  }
done
echo "toolchain: $($CXX -dumpversion) at $(command -v "$CXX")"

CPU=${AIOS_ARM64_CPU:-cortex-a53}

# -mstrict-align is not a nicety. Until metal_mmu_init runs the MMU is off, and
# with it off EVERY access is Device-nGnRnE, where an unaligned or 16-byte pair
# access faults regardless of SCTLR.A. Without this flag GCC spills a struct
# with `str q31, [sp, #296]` and the kernel takes a recursive alignment abort
# inside the device tree parser before it has printed a word.
#
# -ffreestanding, but NOT -mgeneral-regs-only: the compute kernels are NEON and
# the trap path saves q0-q31 precisely so they can be.
FLAGS=(-std=c++20 -O2 -mcpu="$CPU" -mstrict-align
       -ffreestanding -fno-builtin -nostdlib -nostdinc++
       -fno-exceptions -fno-rtti -fno-stack-protector
       -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
       -fno-threadsafe-statics -fno-common
       -DAIOS_METAL=1 -DAIOS_ARM64=1
       -I"$here/include" -I"$here/metal/arm64/include"
       -Wall -Wextra -Wno-unused-parameter)

SRCS=(krt fdt uart rpi_mailbox mmu gic bcm_irq timer interrupts psci smp neon virtio virtio_mmio gpu ramfb fb sdio bcm43438 dhd lan9514 net dwc2 usb_hid sdcard fat32 crashlog process gpio desktop screenshot main)


# ---- incremental + parallel compilation -----------------------------------
# Every unit used to be recompiled serially on every run: 198 translation
# units, one g++ at a time, ~10 minutes for a one-line change. Each compile
# now writes a .d dependency file; a unit is rebuilt only if its source, any
# header it included, or this script is newer than its object. Compiles run
# $JOBS at a time.
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
SELF=$here/metal/build-arm64.sh
is_stale() {  # obj src
  local o=$1 src=$2 d=${1%.o}.d dep
  [ -f "$o" ] && [ -f "$d" ] || return 0
  [ "$src" -nt "$o" ] && return 0
  [ "$SELF" -nt "$o" ] && return 0
  while read -r dep; do
    [ -n "$dep" ] && [ "$dep" -nt "$o" ] && return 0
  done < <(tr -d '\\' < "$d" | tr ' ' '\n' | sed -e '1d' -e '/:$/d' -e '/^$/d')
  return 1
}
cc_unit() {  # flags-array-name src obj
  local flag_name=$1 src=$2 o=$3
  if ! is_stale "$o" "$src"; then return 0; fi
  # wait -n reaps one finished compile; a bare `wait` reaped them all, and the
  # final check then waited on pids that were already gone ("not a child"),
  # which read as a failed compile when every unit had built.
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
    wait -n || { echo "compile failed - see above" >&2; wait; exit 1; }
  done
  if [ "$flag_name" = "FLAGS" ]; then
    ( $CXX "${FLAGS[@]}" -MMD -MF "${o%.o}.d" -c "$src" -o "$o" || { rm -f "$o"; exit 1; } ) &
  elif [ "$flag_name" = "STLFLAGS" ]; then
    ( $CXX "${STLFLAGS[@]}" -MMD -MF "${o%.o}.d" -c "$src" -o "$o" || { rm -f "$o"; exit 1; } ) &
  else
    ( $CXX "${USERFLAGS[@]}" -MMD -MF "${o%.o}.d" -c "$src" -o "$o" || { rm -f "$o"; exit 1; } ) &
  fi
  CC_PIDS+=($!)
  CC_OBJS+=("$o")
}
CC_OBJS=()
CC_PIDS=()

objs=()
for s in "${SRCS[@]}"; do
  f=$here/metal/arm64/$s.cpp
  o=$out/obj/$s.o
  cc_unit FLAGS "$f" "$o"
  objs+=("$o")
done

# ---- the STL-linked half: real src/ modules, real libstdc++.a -------------
# Deliberately NOT reusing FLAGS: -nostdinc++/-ffreestanding/-fno-rtti/
# -mstrict-align/-fno-builtin are exactly what makes SRCS's freestanding
# style build, and exactly what breaks parsing <functional>/<string> and
# real STL codegen. These files only ever run after metal_mmu_init has the
# MMU on, so -mstrict-align's boot-time justification does not apply to them.
STL_GCCLIB=$($CXX -print-file-name=libstdc++.a)
if [ "$STL_GCCLIB" = "libstdc++.a" ]; then
  echo "no static libstdc++.a for $CXX - skipping STL-linked src/ modules (entity, fabric, ...)" >&2
  STL_GCCLIB=""
else
  STL_GCCLIB=$(dirname "$STL_GCCLIB")
  # Deliberately NOT -I"$here/metal/arm64/include": that directory holds
  # freestanding stand-ins (its own cstddef, stdint.h, ...) FLAGS's
  # -nostdinc++ build needs and STLFLAGS must not see - one of them shadows
  # the real aarch64-linux-gnu <cstddef> and its max_align_t, which
  # bits/memory_resource.h (pulled in via <functional> -> <unordered_map>)
  # needs and a hand-written freestanding stand-in was never going to have.
  STLFLAGS=(-std=c++20 -O2 -mcpu="$CPU" -fno-exceptions -fno-stack-protector
            -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
            -fno-threadsafe-statics -DAIOS_METAL=1 -DAIOS_ARM64=1
            -I"$here/include" -I"$here/metal/arm64"
            -Wall -Wno-unused-parameter)
  STLSRCS_METAL=(cxx_runtime hal_metal_arm64 hal_net_arm64 hal_thread_arm64 hal_host_arm64 posix_shim kernel_boot entity_shell fleet_shell node_shell grain_shell desktop_shell)
  # src/ is arch-neutral by construction: metal/build.sh compiles it with this
  # exact find, excluding only the hosted HAL shims (src/hal/, which speak
  # Linux/macOS syscalls and have no bare-metal counterpart) and the hosted
  # intelligence plugins that dlopen a host process. Compiling the identical
  # set here is what makes this a port rather than a courtesy link: anything in
  # src/ that reaches for an x86 primitive now BREAKS this build instead of
  # quietly being left out of it. The per-file object name is the path with
  # separators flattened, so two directories holding the same basename cannot
  # silently overwrite one another in $out/obj.
  # daemon_mgr reaches for real OS process spawn/signal (aios::hal::
  # spawn_process et al) this substrate has no backing for - not a stub to
  # paper over, genuinely not ported yet, same as this find already excludes
  # src/hal/ and the hosted intelligence plugins. Nothing else in src/ calls
  # into daemon_mgr.cpp directly, so excluding it costs the desktop nothing.
  # mysql_binlog.cpp stays IN: sql_database.cpp's binlog replication feature
  # links straight to BinlogConsumer, so compositor.cpp (which calls into
  # SqlDatabase) needs it present even though nothing here uses replication
  # today - see posix_shim.cpp for what mysql_binlog.cpp itself needed
  # (mmap et al) to link on this substrate.
  while IFS= read -r f; do
    s=${f#"$here/src/"}
    o=$out/obj/$(echo "${s%.cpp}" | tr '/' '_').o
    cc_unit STLFLAGS "$f" "$o"
    objs+=("$o")
  done < <(find "$here/src" -name '*.cpp' \
             | grep -vE "/hal/|/intelligence/(plugin|external_process|wire_hosted)\.cpp" \
             | grep -vE "/userland/daemon_mgr\.cpp" \
             | sort)
  STLSRCS_METALLIBS=("$STL_GCCLIB/libstdc++.a" "$STL_GCCLIB/libsupc++.a" "$STL_GCCLIB/libgcc_eh.a")
  # The metal-side STL half: cxx_runtime supplies the C-ABI floor the framed STL
  # pulls in (memchr/strtod/__libc_single_threaded/malloc/abort/snprintf) and the
  # four hal_*_arm64 units supply aios::hal::{Mutex,Thread,sleep_ms,net_*,...}.
  # These were declared above but never compiled or appended to objs, so the
  # full-userland link resolved against nothing. Compile and link them here.
  for s in "${STLSRCS_METAL[@]}"; do
    cc_unit STLFLAGS "$here/metal/arm64/$s.cpp" "$out/obj/$s.o"
    objs+=("$out/obj/$s.o")
  done
fi
# Every compile above must have landed - and succeeded - before anything links.
# A failed unit removes its own .o so a stale object can never paper over it.
# A pid the throttle already reaped cannot be waited on again, so the verdict is
# the objects themselves: a failed unit removed its own .o.
wait
for o in "${CC_OBJS[@]}"; do [ -f "$o" ] || { echo "compile failed - see above ($o missing)" >&2; exit 1; }; done

# The programs the machine ships with: real freestanding AArch64 ELF
# executables, no libc, linked where user space lives (0x40_0000_0000 — above
# every physical address this machine has, so a user VA can never alias a
# kernel one). They are embedded as bytes and loaded by the kernel's own ELF
# loader, so running one takes the same path a real program would.
UFLAGS=(-std=gnu11 -O2 -mcpu="$CPU" -ffreestanding -nostdlib -static -fno-pic -fno-pie
        -fno-stack-protector -fno-asynchronous-unwind-tables -Wall)
USER_VA=0x4000000000

{
  echo '/* generated by metal/build-arm64.sh — do not edit */'
  echo '.section .rodata'
  for u in hello spin crash priv neon shell; do
    $CC "${UFLAGS[@]}" -Wl,-e,_start -Wl,-Ttext="$USER_VA" \
        -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
        -o "$out/user/$u" "$here/metal/arm64/user/u_$u.c"
    echo ".balign 16"
    echo ".globl user_${u}_start"; echo "user_${u}_start:"
    echo ".incbin \"$out/user/$u\""
    echo ".globl user_${u}_end"; echo "user_${u}_end:"
  done
} > "$out/obj/user_blobs.S"

$CC -mcpu="$CPU" -c "$here/metal/arm64/boot.S"   -o "$out/obj/boot.o"
$CC -mcpu="$CPU" -c "$here/metal/arm64/vectors.S" -o "$out/obj/vectors.o"
$CC -mcpu="$CPU" -x assembler-with-cpp -c "$out/obj/user_blobs.S" -o "$out/obj/user_blobs.o"
$CC -mcpu="$CPU" -I"$here" -c "$here/metal/arm64/bcm43438_blobs.S" -o "$out/obj/bcm43438_blobs.o"
$CC -mcpu="$CPU" -I"$here" -c "$here/metal/arm64/app_blobs.S" -o "$out/obj/app_blobs.o"

STLLIBS=()
if [ -n "$STL_GCCLIB" ]; then
  STLLIBS=("$STL_GCCLIB/libstdc++.a" "$STL_GCCLIB/libsupc++.a" "$STL_GCCLIB/libgcc_eh.a")
fi

$LD -T "$here/metal/arm64/link.ld" -nostdlib --no-warn-rwx-segments \
    -o "$out/aios-metal-arm64.elf" \
    "$out/obj/boot.o" "$out/obj/vectors.o" "$out/obj/user_blobs.o" "$out/obj/bcm43438_blobs.o" "$out/obj/app_blobs.o" "${objs[@]}" \
    "${STLLIBS[@]}" \
    "$($CC -mcpu="$CPU" -print-libgcc-file-name)"

$OBJCOPY -O binary "$out/aios-metal-arm64.elf" "$out/aios-metal-arm64.bin"
# The same objects linked for QEMU virt, whose DRAM starts at 0x40000000 (see
# link.ld): the Pi image above is linked at 0x80000 and is not relocatable.
# --defsym has to come BEFORE -T: link.ld's DEFINED() only sees symbols defined earlier.
$LD --defsym KERNEL_BASE=0x40080000 -T "$here/metal/arm64/link.ld" -nostdlib --no-warn-rwx-segments \
    -o "$out/aios-metal-arm64-virt.elf" \
    "$out/obj/boot.o" "$out/obj/vectors.o" "$out/obj/user_blobs.o" "$out/obj/bcm43438_blobs.o" "$out/obj/app_blobs.o" "${objs[@]}" \
    "${STLLIBS[@]}" \
    "$($CC -mcpu="$CPU" -print-libgcc-file-name)"
# Booted as a flat Image (its header is at the front), QEMU puts it at RAM base
# + text_offset AND hands it the device tree in x0. As an ELF it gets no DTB,
# finds no UART, and dies silently.
$OBJCOPY -O binary "$out/aios-metal-arm64-virt.elf" "$out/aios-metal-arm64-virt.bin"

undef=$($NM -u "$out/aios-metal-arm64.elf" 2>/dev/null | wc -l | tr -d ' ')
echo "kernel: $(du -h "$out/aios-metal-arm64.elf" | cut -f1) elf, $(du -h "$out/aios-metal-arm64.bin" | cut -f1) flat, $undef undefined symbols"

# Publish to the netboot TFTP root, if this machine serves one, so a build is
# the whole deploy: power-cycle the Pi and it fetches this kernel. Only a
# kernel that linked with zero undefined symbols is published, and the copy
# is atomic (temp + rename) so a Pi fetching mid-build never gets a torn file.
TFTP_ROOT=$here/build/tftpboot
if [ -d "$TFTP_ROOT" ] && [ "$undef" = "0" ]; then
  cp "$out/aios-metal-arm64.bin" "$TFTP_ROOT/.kernel8.img.tmp"
  mv -f "$TFTP_ROOT/.kernel8.img.tmp" "$TFTP_ROOT/kernel8.img"
  echo "netboot: published to $TFTP_ROOT/kernel8.img ($(sha256sum "$TFTP_ROOT/kernel8.img" | cut -c1-16))"
fi

# A disk for the virtio-mmio block driver to actually talk to.
IMG=$out/aios-metal-disk-arm64.img
[ -f "$IMG" ] || dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
echo "disk:   $IMG"

if [ "${1:-}" = "--run" ]; then
  shift
  exec qemu-system-aarch64 \
    -M virt -cpu "$CPU" -m 2G -smp 4 -nographic -no-reboot \
    -kernel "$out/aios-metal-arm64-virt.bin" \
    -drive file="$IMG",if=none,id=drv0,format=raw \
    -device virtio-blk-device,drive=drv0 \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
    "$@"
fi
