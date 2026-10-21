# Bare metal and installing AIOS

AIOS runs in four places: hosted on Linux or macOS, as its own kernel on
x86-64 hardware with no host OS, as its own kernel on a Raspberry Pi 3
(AArch64), and as the PID 1 of a minimal Linux that serves as the hardware
host. This page covers the last three, plus every way to get AIOS onto a
machine.

| path | what boots | script | persistent state on |
|---|---|---|---|
| x86-64 bare metal, ISO | `aios-metal.elf` (Multiboot2) via GRUB | `metal/build.sh` | a virtio-blk or NVMe disk after `host volume format`, otherwise RAM |
| x86-64 bare metal, disk image | the same kernel, GRUB on a FAT partition | `metal/mkdisk.sh` | the image's AOS partition (type `0x7F`), formatted on first boot |
| dual boot next to Linux | the same kernel from the host's own GRUB | `scripts/install-dualboot.sh` | a virtio-blk or NVMe disk, same rules as above |
| USB stick | the bare-metal ISO written raw | `scripts/install-dualboot.sh disk <dev>` | see [USB stick](#usb-stick) |
| installable ISO (Linux host) | a Linux kernel with `aios-boot` as PID 1 | `boot/mkimage.sh` | ext4 partition labelled `AIOS` after `host install` |
| Raspberry Pi 3 | `kernel8.img` = `aios-metal-arm64.bin` | `metal/build-arm64.sh` | FAT32 on the SD card (`/sd/`) |

What has been shown to work on screen already lives in these folders, which
this page links to rather than repeats:

- [docs/metal-desktop/](metal-desktop/README.md): the x86 framebuffer desktop, driven through PS/2 mouse and keyboard (`metal/drive-desktop.py`).
- [docs/metal-canvas/](metal-canvas/README.md): the windowless lifecycle canvas (`metal/drive-canvas.py`).
- [docs/metal-horizon/](metal-horizon/README.md): standing directives shown on the canvas (`metal/drive-horizon.py`).

> **Both bare-metal kernels build and boot from this tree:**
>
> - x86-64: `metal/build.sh --run` builds the kernel and ISO and boots it in QEMU:
>   `28/28 checks passed … ACCEPTANCE: PASS — the kernel ran with no Linux underneath`
>   (four cores, a parallel matmul on the application processors, a checkpoint on
>   the persistent root, the console refused authority it does not hold).
> - Raspberry Pi 3: `metal/build-arm64.sh` produces
>   `build/metal-arm64/aios-metal-disk-arm64.img`; `metal/drive-pi-things.py` boots it
>   under QEMU raspi3b, types SQL on an emulated USB keyboard and reads the serial
>   log: the kernel boots, ASQL answers, and a live Thing moves from `s=7` to `s=17`
>   on an insert, with its history.
>
> Some command-surface sessions below were first driven on the kernel installed at
> `/boot/aios-metal.elf` (built 2026-09-08); they are marked where they appear.

---

## x86-64 bare metal

### Host tools

| need | for | Debian/Ubuntu package |
|---|---|---|
| an x86-64 `g++` that accepts `-mcmodel=kernel -mno-red-zone` | the kernel | `g++` |
| `gcc`, `ld`, `objcopy` | the ring-3 programs in `metal/user/`, the AP trampoline | `gcc`, `binutils` |
| `grub-mkrescue`, `xorriso`, `mtools` | the ISOs | `grub-common`, `grub-efi-amd64-bin`, `xorriso`, `mtools` |
| `/usr/lib/grub/i386-pc` | BIOS boot as well as UEFI | `grub-pc-bin` |
| `grub-mkimage`, `mkfs.fat`, `mmd`/`mcopy`, `python3` | `metal/mkdisk.sh` | `grub-common`, `dosfstools`, `mtools` |
| `qemu-system-x86_64` | `--run` and every `drive-*.py` | `qemu-system-x86` |
| OVMF | UEFI boots in QEMU | `ovmf` |
| `tmux`, `nc` | `metal/aos-vm.sh` | `tmux`, `netcat-openbsd` |
| `node` | `metal/mind-wire.py` (runs `intelligence/assay/bridge.cjs`) | |

Without the `i386-pc` GRUB modules the ISO and disk image are UEFI-only, and
the scripts say so. `metal/qemu_common.py` then picks OVMF from
`/usr/share/ovmf/OVMF.fd`, `/usr/share/OVMF/OVMF_CODE_4M.fd`, the edk2 paths
or Homebrew's.

### Build and boot in QEMU

    metal/build.sh            # build/metal/aios-metal.elf + two ISOs
    metal/build.sh --run      # ...then boot the acceptance ISO in QEMU; exit 0 on PASS

`build.sh` compiles every `src/**/*.cpp` except the hosted HAL (`src/hal/`
other than `volume.cpp` and `fabric.cpp`) and the hosted intelligence plugins.
It also compiles the `metal/*.cpp` HAL and the `cli/*.cpp` command surface,
using `-DAIOS_METAL=1 -nostdlib -mno-red-zone`, and links them against
`libstdc++.a`. It is incremental: an object is rebuilt when it is older than
its source, the newest header, or `build.sh`. `JOBS=N` caps the parallel
compiles. On a shared 4-core box, `JOBS=2` is a reasonable setting.

Outputs in `build/metal/`:

- `aios-metal.elf`: the kernel.
- `aios-metal.iso`: GRUB menu entry 0 is the command surface (3 s timeout). Entry 1 runs the acceptance sequence and halts.
- `aios-metal-accept.iso`: the same menu with the acceptance entry as default, timeout 0.

`--run` boots the acceptance ISO with
`qemu-system-x86_64 -cpu max -smp 4 -m 512 -nographic -device isa-debug-exit,iobase=0xf4`.
It treats QEMU exit code 33 (the kernel writing `0x10` to the debug-exit
port) as PASS. It refuses to boot an ISO that is older than the ELF it just
linked.

**Driven here**, using the installed `/boot/aios-metal.elf` on a scratch ISO
with the same GRUB entry and the same QEMU line as `--run` (trimmed):

```
[metal] AIOS bare-metal entry; multiboot2 magic ok
[metal] display: framebuffer 1920x1080x32, text 240x67
[metal] interrupts: IDT live, PIC remapped to 32-47, timer at 100 Hz, PS/2 on IRQ1 and IRQ12
[metal] interrupts: moved to the IOAPIC, both 8259s masked, acknowledged at the local APIC
[metal] cpus online: 4
[metal] heap 504 MiB, tsc 3796 MHz, usable memory 511 MiB
[metal] no disk: state lives in memory
[metal] no nic
kernel   72fea2b52596393a000972f15fa9d794
host     x86_64 · bare-metal · 4 cpu · 511 MiB
acceptance
  ok    graph scheduled and executed on the CPU device (2 ops, 37517 µs)
  ok    matmul [58 64 139 154], reduce 415
  ok    parallel matmul on 4 cores correct (C[0]=64.000)
  ok    work ran on application processors (4 AP jobs, 3 cores)
  ok    checkpoint written to the persistent root /aios/root/checkpoints/.../1.aoc
  ok    entity restored with the SAME identity
  ...
  ok    the refusals are on the kernel's persistent record (4)
28/28 checks passed · heap peak 97593 KiB · uptime 266 ms
ACCEPTANCE: PASS — the kernel ran with no Linux underneath
qemu exit 33
```

### UEFI and BIOS

Both ISOs come from `grub-mkrescue`. They boot on UEFI, and also on BIOS when
`grub-pc-bin` is installed. The GRUB config does three things: it puts the
terminal on serial (115200 8N1) and the console together, asks for a graphics
mode (from 1920x1080x32 down to `auto`) with `gfxpayload=keep`, and loads the
kernel with `multiboot2`. The kernel's Multiboot2 header asks for a linear
framebuffer. Under UEFI there is no VGA text mode, so the console is drawn into
that framebuffer with the 8x16 font in `metal/font8x16.h`. Under BIOS it falls
back to VGA text if no framebuffer is offered.

`metal/screenshot.py ISO OUT.png [--bios]` boots under OVMF (or BIOS),
screendumps the framebuffer through the QEMU monitor, checks the glyphs, types
`help`, and writes PNGs. Not driven here.

### SMP

`metal/smp.cpp` reads the CPU list from the ACPI MADT and starts the
application processors with INIT-SIPI-SIPI through the real-mode trampoline in
`metal/ap_trampoline.S`. The trampoline is linked flat at 0 and embedded as
bytes. Interrupts then move from the 8259 PICs to the IOAPIC when ACPI lists
one. `hal::Thread` spreads scheduler waves and the CPU device's `parallel_for`
across the cores. `nproc` reports the core count. In the run above, QEMU's
`-smp 4` showed up as `cpus online: 4`, and the matmul ran on the APs.

### Devices the x86 kernel drives

Taken from the probes in `metal/main.cpp`:

- **Storage:** the first virtio-blk disk, or else an NVMe controller (`metal/virtio_blk.cpp`, `metal/nvme.cpp`). There is no AHCI/SATA and no USB mass-storage driver.
- **Network:** virtio-net, or else an Intel 8254x (`metal/e1000.cpp`).
- **Input:** PS/2 keyboard (IRQ1) and mouse (IRQ12, including 4-byte wheel packets). USB controllers show up only as a class name in the PCI scan.
- **Display:** the framebuffer GRUB hands over, or a virtio-gpu that the kernel brings up itself (`metal/virtio_gpu.cpp`, see `desktop-09-virtio-gpu.png`).
- **Serial:** COM1 is the console. COM2 carries the mind wire (`metal/wire_mind.cpp`).
- **Clock:** the CMOS real-time clock, read as UTC at the first ask and carried forward on the TSC (`metal/hal_metal.cpp`). `date`, ASQL's `NOW()` and a table's `CREATE_TIME` read it; `metal/drive-clock.py ISO [cpus]` checks all three against the host's clock under QEMU (`-rtc base=utc`). The calendar, time zones, `strftime` and `clock_gettime` are `metal/time_common.inc`, shared with the Pi; a machine whose RTC holds no valid date counts up from the build's epoch, and `metal_wall_clock_set()` says so.
- **Thread-local storage:** each core gets its own copy of the kernel's TLS image, and `FS` points at it (variant II, `metal_make_tls_block` in `metal/main.cpp`); the boot core's is in place before any constructor runs, each AP's before it is woken.
- **Floating point in text:** `printf`'s `%f`/`%e`/`%g` print the exact decimal value rounded half-to-even, and `strtod` is correctly rounded (`metal/float_common.inc`, shared with the Pi). `metal/drive-sql.py` checks doubles through ASQL.

### The command surface and the `host` group

The prompt is `aios>`, and it is the same `cli::Session` the hosted build
uses. Commands that touch the machine itself live under `host` (`host help`
lists them). They are implemented in `metal/main.cpp`; `cli/commands.cpp`
only dispatches to them.

| command | does |
|---|---|
| `host disks` | the disk, its AOS partition if any, whether it holds a mounted volume; a whole disk with no volume is reported `blank` or `not an AOS disk: holds …` (a GPT, an MBR partition table and its types, a boot record, data in its first MiB or last sectors) |
| `host volume` | volume stats (halves, generation, records, log use, compactions) |
| `host volume format [label] [confirm=<disk>]` | writes an AOS volume on the AOS partition, or on the whole disk when there is no partition, and moves the live root onto it. A **blank** whole disk is claimed without asking. A whole disk that holds anything else (see `host disks`) is refused unless the console holds a single-use `kernel:destroy` capability for the named disk **and** names the disk back with `confirm=<disk>` (e.g. `confirm=virtio-blk`); overwriting an existing AOS volume also needs `kernel:destroy`. The console takes it with `cap request kernel <disk> destroy confirm=<disk>`: granted to the console alone, single use, lapsing after five minutes unused, consumed by the format, and on the authority log |
| `host net` | NIC, MAC, fabric frame counts |
| `host net up [ip mask gw dns]` | AOS's own TCP/IP. With no arguments it uses DHCP |
| `host net status \| ping <ip> \| resolve <host> \| get <host> [path] [port] \| btc [seed]` | over that stack |
| `host desktop` / `desktop` / `gui` | the framebuffer desktop ([docs/metal-desktop](metal-desktop/README.md)) |
| `host canvas` | the lifecycle canvas ([docs/metal-canvas](metal-canvas/README.md)) |
| `host dash` | live dashboard |
| `host run <p>... [bare]`, `host spawn <p>`, `host wait`, `host ps`, `host kill <pid> [sig]` | ring-3 ELF programs from `/bin` (`hello probe crash priv peek spin fs mem forker execer sig fpu edges`, from `metal/user/`). `host run` waits for and reaps only the programs it started; `host wait` waits for all of them. `/bin/edges` probes the system-call boundary (see below) |
| `host irq`, `host heap` | interrupts and the tick; heap use |
| `host reboot`, `host poweroff` | also available without the `host` prefix |

`install` on bare metal only reports. It prints `Installer: NOT PRESENT --
this kernel cannot install itself to a disk`, and `install dualboot`/`disk`
print that it needs the hosted side.

**Driven here** (installed kernel, 64 MiB scratch virtio disk, SLIRP
virtio-net, `-smp 2`):

```
[metal] disk virtio-blk 64 MiB · no AOS volume (host volume format)
[metal] nic virtio-net 52:54:00:12:34:56 · AOS fabric + TCP/IP ready
aios> host disks
virtio-blk  64 MiB  whole disk: no volume
aios> host volume format
volume 'aios' formatted on virtio-blk; the persistent root now lives on it (19 records carried over)
aios> host volume
volume 'aios' on virtio-blk: 64 MiB, half 31 MiB, active half 0, generation 20, 19 records, 14 files, 119 KiB live, 127 KiB of log used, 0 compactions
aios> host net up
bringing the interface up (DHCP)…
up: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2 dns 10.0.2.3
aios> host net ping 10.0.2.2
reply from 10.0.2.2: 1663 µs
aios> nproc
2
```

After a power cycle on the same disk:

```
[metal] disk virtio-blk 64 MiB · AOS volume 'aios' mounted (19 records, generation 20)
aios> host disks
virtio-blk  64 MiB  whole disk: AOS volume 'aios' (mounted: persistent root)
```

A disk that holds someone else's data (here an MBR with a Linux partition and
a payload behind it) is refused, and its bytes are untouched afterwards:

```
aios> host disks
virtio-blk  64 MiB  whole disk: no volume, not an AOS disk: holds an MBR partition table (types 0x83)
aios> host volume format
error: denied: virtio-blk holds an MBR partition table (types 0x83); formatting erases the whole disk — repeat with confirm=virtio-blk to wipe it
aios> host volume format confirm=virtio-blk
error: denied: the kernel refuses: virtio-blk is not an AOS disk (it holds an MBR partition table (types 0x83)) and formatting would erase the whole disk; that needs kernel:destroy over it: cap request kernel virtio-blk destroy confirm=virtio-blk
```

The operator takes that capability for the one disk, and spends it:

```
aios> cap request kernel virtio-blk destroy confirm=virtio-blk
the kernel permits it: capability 764922371977431a000a061b6ee20143 kernel:3c90c64fdf6a destroy
aios> cap mine
764922371977  386b3b7a8af8  kernel:3c90c64fdf6a   destroy                 uses=0/1  expires=+298.127 s
aios> host volume format confirm=virtio-blk
volume 'aios' formatted on virtio-blk; the persistent root now lives on it (20 records carried over)
aios> host volume format confirm=virtio-blk
error: denied: the kernel refuses: virtio-blk already holds an AOS volume (the persistent root); wiping it needs kernel:destroy over it: cap request kernel virtio-blk destroy confirm=virtio-blk
```

The capability is gone after one format. It is granted only to the console,
only when the request names the disk twice, and lapses after five minutes
unused; a mind or any other resident is refused, and `cap request kernel *
destroy` (the whole machine) stays refused for everyone.

An `entity create` made before `host poweroff` was not in `entity list` after
the reboot, and the record count had not moved. The persistence drives
(`metal/drive-storage.py`, `metal/drive-disk.py`) prove survival with
`store put` objects and the kernel identity, not with entities.

### Deployable disk image

    metal/mkdisk.sh [size-MiB]       # default 256 → build/metal/aios-metal-disk.img

The image is MBR-partitioned. Partition 1 is a 64 MiB FAT16 (`AIOSBOOT`, type
`0xEF`) holding `/boot/aios-metal.elf`, `grub.cfg` and `EFI/BOOT/BOOTX64.EFI`.
Partition 2 is type `0x7F`, which the kernel formats as an AOS volume on first
boot. With `i386-pc` modules, GRUB's `boot.img` goes in the MBR and `core.img`
at LBA 1, so the image boots on BIOS and UEFI. It needs `build/metal/aios-metal.elf`
first. To use it under another hypervisor, convert it with
`qemu-img convert -O vmdk|vhdx`.

`metal/drive-disk.py IMG [--uefi]` boots it twice and checks that the identity
and stored state come back. Not driven here, because no kernel could be built.

### Running a long-lived VM: `metal/aos-vm.sh`

    metal/aos-vm.sh up | down | status | console | send "<line>" | ask "<q>"
    metal/aos-vm.sh gui off|gtk|cocoa|vnc|auto
    metal/aos-vm.sh mind <provider> <model> | mind off
    metal/aos-vm.sh screen [out.png] | build | upgrade | newdisk | disk <path> | config

It boots `~/aos/disk.img` (copied from `build/metal/aios-metal-disk.img` by
`newdisk`) in a tmux session called `aios`. It uses KVM when `/dev/kvm` is
writable. COM2 goes to `/tmp/aios-mind.sock` and the monitor to
`/tmp/aios-monitor.sock`. Settings persist in `~/.config/aos-vm.conf`.
`upgrade` writes a fresh boot partition and keeps the AOS partition (the
persistent root) byte for byte, and it refuses if the partition geometry
differs. `build` runs `git pull`, the hosted cmake build, `metal/build.sh`
and `metal/mkdisk.sh`. Not driven here.

Other helpers:

- `metal/console.py start --disk IMG [--iso ISO] [--smp N] [--mem M] [--mind SOCK]`, then `send "<line>"`, `read`, `stop`. This is a serial proxy for one exchange at a time; state is kept in `$AIOS_CONSOLE_DIR` (default `/tmp/aios-console`).
- `metal/mind-wire.py SOCK` connects COM2 to `intelligence/assay/bridge.cjs`. Then `mind bind mind wire` inside the machine attaches a model. Provider and model come from `~/.assay/config.json` or `AIOS_MODEL_PROVIDER`/`AIOS_MODEL`.
- `metal/prove-baremetal.sh` waits for a running `metal/build.sh`, refuses a kernel older than the newest source, boots it under `qemu-system-x86_64` (from `PATH`, else Homebrew's; `QEMU=` overrides) and appends the run to `build/metal/prove-baremetal.log` ending `SETTLE: PASS|FAIL`. It runs on Linux and macOS.

#### Processes, the idle learner, and the system-call edges

The idle learner (`jepa learn`) runs a measured workload — two `/bin/spin`
tasks — every 200 ticks while the console is idle. It waits for and reaps only
its own two tasks, their output is discarded, and it does not run at all while
a program someone started is still live, so user programs keep their exit
status for `host wait` (`metal/drive-process.py` passes 44/44).

`host run /bin/edges` exercises the system calls at their edges:

```
jepa shm: -1                                     (no kernel address is ever handed to user space)
mmap wrapping length: -1, next mmap at 550292684800   (a length that would wrap is refused)
jepa state, wrapped count at the stack top: -1   (count*4 is range-checked without overflow)
write from an unmapped address: -1               (ring 0 checks every page is mapped, and writable when it writes)
nested signal: held until the first handler returned, then delivered
```

### Two kernels on one wire

    python3 metal/drive-fabric.py build/metal/aios-metal.iso

This starts two QEMU guests joined by a `-netdev socket` pair on a free
127.0.0.1 port. B runs `node serve 47311 lab-shared-secret`. A runs
`node connect 52:54:00:aa:bb:02 47311 lab-shared-secret`, which attaches over
the AOS fabric (raw Ethernet, no IP). A then allocates memory on B, runs a
matmul graph on `dev=remote-cpu`, and expects `[58 64 139 154]` back. Not
driven here.

`metal/drive-net.py [ISO]` drives the TCP/IP stack under SLIRP. It requires
DHCP and a gateway ping; DNS, HTTP GET and the Bitcoin handshake are reported
when the sandbox allows outbound traffic. `AOS_NIC=e1000` exercises the Intel
driver. Other drives in `metal/` (`drive-shell.py`, `drive-process.py`,
`drive-storage.py`, `drive-remote.py`, `drive-power.py`, `drive-sql.py`, …)
each describe themselves in their header.

---

## Installing on x86-64

### Dual boot next to an existing Linux (`scripts/install-dualboot.sh`)

    scripts/install-dualboot.sh status      # read-only: kernel, /boot copy, GRUB entry, lsblk
    scripts/install-dualboot.sh dualboot    # install (aliases: install, host)
    scripts/install-dualboot.sh uninstall   # remove (alias: remove)

**`dualboot` and `uninstall` run `sudo`.** `dualboot` does the following:

1. Builds `build/metal/aios-metal.elf` if it is missing. It does *not* rebuild a stale one.
2. `sudo cp` the kernel to `/boot/aios-metal.elf`, and to `/boot/efi/EFI/AIOS/aios-metal.elf` if `/boot/efi/EFI` exists.
3. Writes `/etc/grub.d/42_aios` with two entries, "AIOS — Bare Metal (Interactive Desktop)" and "… (Acceptance Diagnostic & Self-Test)". Both `search --fs-uuid` for the filesystem that holds `/boot`, and name the kernel by its path **on that filesystem** (from `findmnt`): `/boot/aios-metal.elf` when `/boot` is a directory of the root filesystem, `/aios-metal.elf` when `/boot` is its own partition, `/@/boot/aios-metal.elf` on a btrfs root mounted from subvolume `@`.
4. Regenerates `grub.cfg` with whichever tool the host has: `update-grub` (Debian/Ubuntu), else `grub2-mkconfig -o /boot/grub2/grub.cfg` (Fedora/RHEL/openSUSE), else `grub-mkconfig -o /boot/grub/grub.cfg` (Arch).

`--dry-run` (anywhere on the line, or `AIOS_DUALBOOT_DRY_RUN=1`) changes
nothing: it prints the entry text it would install and each privileged
command. The detection can be pinned to check another layout from this one
(`AIOS_BOOT_FS_TARGET`, `AIOS_BOOT_FS_SOURCE`, `AIOS_BOOT_FS_UUID`,
`AIOS_BOOT_FS_TYPE`, `AIOS_BOOT_FS_ROOT`, `AIOS_GRUB_TOOL`, `AIOS_GRUB_CFG`,
`AIOS_BOOT_DIR`). Driven here, a separate `/boot` on a grub2 distro:

```
$ AIOS_BOOT_FS_TARGET=/boot AIOS_BOOT_FS_SOURCE=/dev/sda1 AIOS_BOOT_FS_UUID=1111-2222 \
  AIOS_BOOT_FS_TYPE=ext4 AIOS_GRUB_TOOL=grub2-mkconfig scripts/install-dualboot.sh --dry-run dualboot
[AIOS INSTALL] Detected /boot on /dev/sda1 (UUID=1111-2222, FS=ext4, mounted at /boot); GRUB path /aios-metal.elf; regenerate with grub2-mkconfig
would run: sudo cp …/build/metal/aios-metal.elf /boot/aios-metal.elf
    |     search --no-floppy --fs-uuid --set=root 1111-2222
    |     multiboot2 /aios-metal.elf
would run: sudo grub2-mkconfig -o /boot/grub2/grub.cfg
```

The generated entries pass `grub-script-check`. On this machine (no
overrides) it detects `/boot` on the root ext4 and keeps `/boot/aios-metal.elf`
and `update-grub`.

The hosted command surface wraps the same script: `install` / `install status`
(read-only), `install dualboot`, `install disk <dev>`, `install remove`.
`install disk` accepts only a `/dev/` path made of letters, digits and
`/ - _ . :` (no `..`) and runs the script with `fork`/`exec`, never through a
shell; a failing script is reported with its real exit status. It
finds the script through `$AOS_INSTALL_SCRIPT`, `scripts/install-dualboot.sh`,
`/usr/local/lib/aios/install-dualboot.sh` or `/repos/aios/scripts/install-dualboot.sh`.
`install status` reports when the `/boot` copy is older or a different size
than the tree's build, because re-running `dualboot` is the only thing that
refreshes it.

The build it compares against is `build/metal/aios-metal.elf` under the
working directory, else `metal/aios-metal.elf` beside the running binary.
Driven here, read-only only (the script itself was not run). This machine
already has it installed, from an older build:

```
$ build/aios exec 'install status'      # from /repos/aios
  /boot Kernel    : INSTALLED (/boot/aios-metal.elf, 5.02 MiB)  STALE — the build at /repos/aios/build/metal/aios-metal.elf is 14.44 MiB
  UEFI copy       : STALE …
```

With no build to compare against, the UEFI line reads `PRESENT (no build to
compare against)`.

On the booted machine, the kernel has the same device support as under QEMU.
It only sees virtio-blk or NVMe disks and virtio-net or Intel 8254x NICs. Do
not run `host volume format` there unless you mean it, as described under
[USB stick](#usb-stick).

### USB stick

    scripts/install-dualboot.sh disk /dev/sdX      # sudo dd of build/metal/aios-metal.iso onto the device

This writes the bare-metal ISO raw. It builds the ISO first only if the ISO is
missing, and it erases the device. The hosted `install disk <dev>` calls the
same thing.

What the stick can do:

- Boot on UEFI, and on BIOS when the ISO was built with `grub-pc-bin`. It gives you the serial and framebuffer console, the desktop, and SMP.

What it cannot do:

- **It cannot install itself.** The bare-metal kernel has no installer. `install` says so, and there is no `host install` on metal.
- **It cannot store state on itself.** The kernel has no USB or SATA driver, so it never sees the stick it booted from. State lives in RAM unless the machine has a virtio-blk or NVMe disk.
- If that NVMe disk has no AOS partition, `host volume format` targets the whole disk. It claims it only if it is blank; a disk holding a partition table or any data (on a real PC, usually the machine's own OS disk) is refused unless the console first takes `kernel:destroy` over that disk (`cap request kernel <disk> destroy confirm=<disk>`) and names it again with `confirm=<disk>`.
- The x86-64 kernel has no USB driver, so its keyboard and mouse must be PS/2 (a VM provides them). The Pi kernel has a USB host driver (`dwc2`) and takes USB keyboards and mice.

`metal/mkdisk.sh`'s image can also be written to a stick or disk. It is the
only bare-metal medium that carries its own AOS partition, but the kernel still
has to reach that partition through virtio-blk or NVMe.

### Installable ISO with a Linux hardware host (`boot/`)

This is a different system from the bare-metal kernel. A stock Linux kernel
boots an initramfs whose PID 1 is `aios-boot` (`boot/boot.cpp`). `aios-boot`
mounts `/proc`, `/sys`, `/dev`, loads storage, Hyper-V and USB HID modules,
brings every interface up with DHCP, and finds or creates the persistent root.
It then keeps `aios --root /aios/root shell` running on the console. Here,
`host` is `aios-boot` itself, passed in through `AIOS_HOST_TOOL`.

Host tools: the hosted build (`build/aios`, `aios-boot`, `aios-mind-peer`,
`mind_reflect.so`, `mind_fast_graph.so`), plus `cpio xz zstd`,
`grub-mkrescue` (`grub-common grub-pc-bin grub-efi-amd64-bin`, which pulls in
`xorriso`/`mtools`), `systemd-boot-efi`, `syslinux-common`, `extlinux`,
`busybox-static`, `dosfstools`, `e2fsprogs`, and `apt-get`/`dpkg-deb` for the
kernel fetch.

    boot/fetch-kernel.sh /tmp/ktree                 # apt-get download linux-image-virtual + modules, extract (no install)
    boot/mkimage.sh build /tmp/ktree [out-dir]      # → <out-dir>/aios.iso (default build/media)
    python3 boot/drive-vm.py build/media [--uefi]   # boot, install to a VM disk, reboot twice, check state

The ISO's GRUB menu has two entries: the display console mirrored to serial,
and a serial console. `aios-boot`'s `host` verbs are:

| verb | does |
|---|---|
| `host disks` | block devices, size, type (`disk`/`usb`/`cdrom`/`part`), detected fs. Marks `(boot media)` and `(AIOS root)` |
| `host install /dev/<disk>` | **erases that disk.** Writes a GPT with a 512 MiB ESP (`AIOS BOOT`, FAT32, legacy-BIOS-bootable bit) and an ext4 root labelled `AIOS`. The ESP gets systemd-boot as `EFI/BOOT/BOOTX64.EFI`, the kernel and initramfs, and syslinux with `extlinux --install` behind `gptmbr.bin`, so the disk boots on UEFI and BIOS. The live session's root (and so its kernel identity) is copied onto the disk. It refuses partitions, the boot media, and disks under 1 GiB |
| `host net` | `ip -brief addr` |
| `host shell` | busybox `sh`; `exit` returns |
| `host media` | the AIOS ISO9660 device |
| `host reboot`, `host poweroff` | sync, unmount, reboot(2) |

On each boot, the `AIOS` ext4 root is checked with `e2fsck -p` and
bind-mounted at `/aios/root`. With no such root, state lives on tmpfs until
`host install`.

`boot/drive-vm.py` is the end-to-end check. It boots the ISO and selects the
serial entry, runs `host install /dev/vda` on a blank 4 GiB disk, then boots
the disk (`boot:` → `serial` on BIOS, the systemd-boot menu on UEFI). It writes
a `store put` object, runs `host reboot`, and boots again to check that the
identity and the object survived. Not driven here: `fetch-kernel.sh` needs
network access to the apt mirrors, and `mkimage.sh` was out of scope for this
pass. The tools it needs are present on this machine.

### Hosted installs

These are not bare metal, but they are the other ways AIOS gets installed:

- `scripts/aos.sh install` supervises a hosted machine and starts it at login (launchd). On Linux, point a `systemd --user` unit at `scripts/aos-session.sh`. See the top-level README.
- `packaging/` builds and installs the portable ASQL server (`install-asql.sh --host <h> [--dry-run]`). See [packaging/README.md](../packaging/README.md).

---

## Raspberry Pi 3 (AArch64)

### Host tools

- An AArch64 cross toolchain. `build-arm64.sh` uses `aarch64-elf-*` when present, else `aarch64-linux-gnu-*` (`gcc-aarch64-linux-gnu g++-aarch64-linux-gnu`). The STL half of the build needs that toolchain's static `libstdc++.a`; without it the `src/` modules are skipped with a message. You can override any tool with `CXX_ARM64`, `CC_ARM64`, `LD_ARM64`, `OBJCOPY_ARM64` or `NM_ARM64`. `AIOS_ARM64_CPU` defaults to `cortex-a53`.
- `qemu-system-aarch64` for `-M virt` or `-M raspi3b`.
- For the card: a reader, `lsblk`, `mount`, `md5sum` and sudo (`flash-rpi3.sh`).
- For netboot: `dnsmasq`, `ip`, and `curl` if the firmware blobs are missing.

On this machine `aarch64-linux-gnu-g++` 13 is installed
(`toolchain: 13 at /usr/bin/aarch64-linux-gnu-g++`).

### Build

    metal/build-arm64.sh          # build/metal-arm64/aios-metal-arm64.{elf,bin} (the Pi, linked at 0x80000)
                                  # + aios-metal-arm64-virt.{elf,bin} (QEMU virt, linked at 0x40080000) + a 64 MiB disk image
    metal/build-arm64.sh --run    # ...then exec qemu-system-aarch64 -M virt -smp 4 -m 2G -nographic on the virt image

The build has two halves.

- **Freestanding substrate:** `metal/arm64/{krt,fdt,uart,rpi_mailbox,mmu,gic,bcm_irq,timer,interrupts,psci,smp,neon,virtio,virtio_mmio,gpu,ramfb,fb,sdio,bcm43438,dhd,lan9514,net,dwc2,usb_hid,sdcard,fat32,crashlog,process,gpio,desktop,screenshot,main}.cpp`. It is built `-ffreestanding -nostdinc++ -mstrict-align`, because the MMU is off at entry and an unaligned access faults.
- **STL half:** every `src/**/*.cpp` that `build.sh` compiles, except `userland/daemon_mgr.cpp`, plus the `cxx_runtime`/`hal_*_arm64`/`*_shell` units. These are linked with the real `libstdc++.a`.

The link also includes the user programs `metal/arm64/user/u_{hello,spin,crash,priv,neon,shell}.c`
(at VA `0x40_0000_0000`), the BCM43430 firmware, NVRAM and CLM blobs from
`metal/arm64/firmware/`, and `app_blobs.S`. Builds are incremental through
`.d` files; `JOBS=N` caps parallel compiles.

If `build/tftpboot/` exists **and** the link has zero undefined symbols,
`build-arm64.sh` also copies `kernel8.img` into it atomically. On a machine
that runs the netboot server, a build is therefore a deploy: the next
power-cycle of the Pi loads it.

Driven here: `metal/build-arm64.sh` builds `build/metal-arm64/aios-metal-disk-arm64.img`,
and `metal/drive-pi-things.py` boots it under QEMU raspi3b (see the state note at the top).
It was not flashed to a card or served over netboot here.

### QEMU targets

- `-M virt` (`build-arm64.sh --run`, `metal/run-arm64-vm.sh`) boots `aios-metal-arm64-virt.bin`. The kernel is not position-independent: the Pi image is linked at `0x80000`, where the Pi firmware puts it, and virt's DRAM starts at `0x40000000`, so the same objects are linked a second time at `0x40080000` and booted as a flat Image (that is what makes QEMU pass the device tree in `x0`). Booting the Pi image on virt printed nothing at all. The Broadcom-only bring-up (BCM interrupt controller, GPIO, VideoCore clock mailbox, DWC2 USB, SDHOST) is skipped when the device tree's UART is outside the BCM peripheral window. It uses virtio-mmio block and net, GIC from the device tree, and PSCI for SMP. `run-arm64-vm.sh` runs detached with a virtio-gpu, VNC on `127.0.0.1:5901` (`AOS_VNC_DISPLAY`), serial on TCP 4444 (`AOS_SERIAL_PORT`) and a monitor socket; `AOS_VM_STATE` (default `/tmp/aos-vm`) prefixes its pidfile, logs and socket. It uses HVF with GICv3 on macOS and TCG `cortex-a72` elsewhere. Subcommands are `status|console|shot FILE|stop`.
- `-M raspi3b` (`metal/drive-pi-things.py`) reproduces the Pi's USB topology with `-device usb-kbd`, the SD card as `if=sd`, and QMP input. Serial input does not reach the kernel on raspi3b, so that drive types through the emulated USB keyboard.

### What the Pi kernel drives

This list covers only what is in `metal/arm64/`:

| hardware | driver | notes |
|---|---|---|
| 4 Cortex-A53 cores | `smp.cpp`, `psci.cpp` | The Pi has no PSCI, so the cores are released through the firmware spin table at `0xd8 + 8*i` (`cpu-release-addr` 0xe0/0xe8/0xf0). Bring-up happens on the `smp` command, or automatically when the desktop starts (`desktop_shell.cpp`). `cpus` reports which cores are up, and `cpus test` runs a job on each |
| interrupts | `bcm_irq.cpp` (BCM2836/7), `gic.cpp` (GICv2/v3 on virt) | |
| framebuffer | `fb.cpp` via the VideoCore mailbox (`rpi_mailbox.cpp`) | Uses the monitor's native mode, because `config.txt` sets no width or height. virtio-gpu and ramfb are for QEMU |
| USB host | `dwc2.cpp` | polled buffer-DMA host mode, no USB interrupt |
| USB keyboard, mouse, hubs | `usb_hid.cpp` | the console reads `usb_get_key()` before serial |
| onboard Ethernet | `lan9514.cpp` (LAN9514, VID 0x0424 PID 0xEC00) | DHCP; `net`, `ping`, `tcp`, `dhcp` |
| Wi-Fi | `sdio.cpp` (Arasan SDHCI on GPIO 34-39) + `bcm43438.cpp` + `dhd.cpp` | `wifi status \| scan \| connect <ssid> [psk] \| disassoc \| log`. Associates open or WPA2-PSK. The firmware is built without a CLM, so `clmload` is required |
| microSD | `sdcard.cpp` (BCM2835 SDHOST, GPIO 48-53) + `fat32.cpp` | FAT32 at `/sd/`, 8.3 names only, no mkdir, long names skipped. `ls`, `fs cat/write/rm`, `sd`, `sd init` |
| UART | `uart.cpp` | GPIO 14/15, 115200 (`enable_uart=1`) |
| GPIO / I2C | `gpio` | `pin`/`gpio`, `pinout` |

At boot the kernel prints a build stamp (`AIOS - AArch64 bare metal [build
<date> <time>]`). It then brings up the MMU, framebuffer, USB, SD (before
networking, so `/sd/wifi.cfg` can be read), and eth0 and wlan0 with DHCP. If a
display is present it drops into the `dash` bring-up dashboard. The prompt is
`aos>` and `help` lists everything. `desktop` runs the windowed compositor.
`grain <src>` evaluates against the running kernel. `fleet H P K` and
`node`/`node start` join a fleet. `console debug on|off` unlocks or locks the
open UDP console.

`node start` serves peers on tcp/7421 that present a token, and the token is
this boot's: `node start <token>`, `node start @FILE` (a file on the card), or
`NODE.KEY` on the card when no argument is given. With none it refuses
(`node: refusing to serve without a token - NODE.KEY is not on the card …`),
and it refuses a token under 16 characters. No token is compiled into the
image, so none is the same on every board or readable from
`kernel8.img`. The automatic start when eth0 gets a lease (`net.cpp`) now
starts the node service only when the card has `NODE.KEY`. The arguments of
`node start` are not written to the on-screen network log.

`info` on the Pi reports `cpus: 4 in the device tree, 1 online (the others
start with `smp` or `desktop`)` after a plain boot: secondary cores are
started on demand, not at boot. The Pi has no GIC and its BCM2836 core-local
controller is not wired to the generic timer, so the timer never interrupts
there: the kernel tells time from the counter and nothing is preempted. `info`
now says that (`timer: 19200000 Hz counter; no tick interrupts …`, `uptime N ms
(counter)`) instead of `100 Hz tick` beside `over 0 ticks`, which read as a
stopped clock.

Wi-Fi credentials are read from `wifi.cfg` on the card (`ssid=`, `psk=`,
`auto_connect=`, `country=`). The template in `metal/arm64/wifi.cfg` is empty.

Screenshots: the board serves a full-resolution QOI on tcp/42427. Fetch it
with `nc -w 20 <pi-ip> 42427 > shot.qoi && metal/qoi2png.py shot.qoi shot.png`.

### Boot from an SD card

    metal/flash-rpi3.sh

This script does the following:

1. Builds the kernel if `.bin` is missing.
2. Finds the boot partition. It looks first at `/media/arch/boot*`, `/run/media/arch/boot*` and `/mnt/rpi_boot`. Otherwise it `sudo mount`s each vfat `/dev/sd[b-z]1` and uses the first one that holds `bootcode.bin`, `start.elf` or `config.txt`.
3. Backs up an existing `kernel8.img` to `kernel8-linux.img`.
4. Restores `bootcode.bin`/`start.elf`/`fixup.dat` from `metal/rpi-firmware/` if any is missing.
5. Copies the kernel as `kernel8.img`, `aios-metal-arm64.bin` and `.elf`, and `wifi.cfg` if the card has none.
6. **Overwrites `config.txt`.**
7. md5-checks the card copy against the build and prints the build stamp it should show.

The `config.txt` it writes sets `arm_64bit=1`, `kernel=kernel8.img`,
`enable_uart=1`, `uart_2ndstage=1`, `core_freq=250`, `hdmi_force_hotplug=1`,
`framebuffer_depth=32` and `disable_commandline_tags=1`. It **also sets
`program_usb_boot_mode=1`**. On an original Pi 3B, the stock firmware then
burns OTP bit 29 on that boot. **This is a permanent change to the board.**
It adds network/USB boot as a fallback and leaves SD boot unchanged. Remove
the line before flashing if you do not want it. Not driven here.

### Netboot (no card swaps)

On the original Pi 3B, the boot ROM's network fetch of `bootcode.bin` is
unreliable. The scripts therefore keep `bootcode.bin` on the card and serve
everything else over TFTP.

1. Convert the card: `metal/strip-sd-to-netboot.sh --yes` moves everything except `bootcode.bin`, `wifi.cfg` and `timeout` into `sdboot-backup/` on the card. It replaces `bootcode.bin` with the vendored one and creates an empty `timeout` file, so `bootcode.bin` retries the network after 10 s. `--restore` moves the files back. `SD_BOOT_MOUNT=<dir>` points it at a mounted image. A `bootcode.bin` with no `start.elf` beside it netboots by itself, and the script states that the OTP fuse is only needed to boot with no card at all.
2. Serve: `metal/serve-netboot.sh` builds, stages the TFTP root (`build/tftpboot/` of this checkout, whatever the checkout is called; `AIOS_NETBOOT_TFTP` overrides) with `kernel8.img`, `start.elf`, `fixup.dat`, `bootcode.bin`, a `config.txt` without the OTP line, and `wifi.cfg`, and finds the host IP on `$AIOS_NETBOOT_IFACE` (default `br0`). It then restarts `aios-netboot.service` if that unit exists. Otherwise it runs `sudo dnsmasq` as ProxyDHCP plus TFTP (`--dhcp-boot=bootcode.bin --pxe-service=0,"Raspberry Pi Boot"`). `--dry-run` prints the paths and the command it would run and changes nothing. Any other argument (`--help`, a typo) prints the usage and exits 2: it used to fall through to a real stage-and-restart.
3. The persistent form is a system unit rendered from `metal/aios-netboot.service.in`: `AIOS_NETBOOT_PI_MAC=… AIOS_NETBOOT_PI_IP=… AIOS_NETBOOT_GATEWAY=… metal/serve-netboot.sh --unit > aios-netboot.service` (optional `AIOS_NETBOOT_IFACE`, `AIOS_NETBOOT_TFTP`, `AIOS_NETBOOT_NETMASK`, `AIOS_NETBOOT_DNS`, `AIOS_NETBOOT_USER`; it refuses to render with a required value missing). It runs dnsmasq in static-DHCP mode for that one MAC (address and boot info in one offer, every other host ignored) because a router that ignores the boot ROM's DISCOVERs stalls a ProxyDHCP-only setup.

After that, `metal/build-arm64.sh` publishes `kernel8.img` into
`build/tftpboot/` (when that directory exists), and a power-cycle loads it.
Not driven here: it would need a Pi, root for dnsmasq, and systemd.

#### The board as it runs today (an example, not a default)

`program_usb_boot_mode`) and `wifi.cfg`. The card was converted with
`strip-sd-to-netboot.sh` and carries the OTP bit from a `program_usb_boot_mode=1`
boot. A kernel reaches the board by being published to that TFTP root and the
board being reset.

At boot the board joins the fleet from `FLEET.CFG` on its card
(`<hub> <port> @FLEET.KEY`, retried on a backoff until it is in). Its UDP console
is locked every boot (`console debug on` opens it to the LAN).

**Driving the board.** `tools/grain_remote.py` talks to it; set where it lives:

```
export AIOS_PI=<board ip>            # or --pi; found by its beacon when unset on the LAN
export AIOS_FLEET_HUB=<hub ip>       # AIOS_FLEET_HUB_PORT, default 7420; for --fleet
python3 tools/grain_remote.py --fleet shell info      # over the authenticated fleet link
python3 tools/grain_remote.py --fleet shell reset     # reboot it onto the published kernel
```

`metal/tools/pi-grain.py`, `tools/pi-net-check.py` (`AIOS_ROUTER`: a LAN host to
test against) and `tools/proof/pi/*.sh` take the same `AIOS_PI`. `info` on a board
running a kernel built from this tree:

```
machine    : Raspberry Pi 3 (Hardware Substrate)
substrate  : AArch64 EL1, 4 KiB granule, 39-bit VA, TTBR0 only
dram       : 0x0000000000000000 .. 0x000000003b000000 (944.0 MiB)
mmu        : on
timer      : 19200000 Hz counter; no tick interrupts (no GIC on this board - time is read from the counter, nothing is preempted)
cpus       : 4 in the device tree, 1 online (the others start with `smp` or `desktop`)
display    : 1280 x 720, Raspberry Pi VideoCore Mailbox HDMI Framebuffer
psci       : absent
uptime     : 30566 ms (counter)
```

---
