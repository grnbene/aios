# AOS-010 — Zero-Host Contract

AOS-010 is a demolition, not a feature. The question it answers is: *what does
the hardware host still provide that the kernel secretly depends on?* The
answer must be machine-generated, and every removal must be measurable.

## The contract

The **kernel core** — everything under `src/` except `src/hal/` (the host
boundary) and the host-specific intelligence adapters (`plugin.cpp`,
`external_process.cpp`) — may not depend on:

    libc (beyond the freestanding primitives below)
    libstdc++ hosted facilities: iostream, locale, filesystem, thread, mutex, chrono clocks
    POSIX             syscalls          Linux headers
    Linux networking  Linux filesystem  Linux threading
    Linux processes   Linux virtual memory   Linux device APIs

Anything the kernel needs from the machine goes behind `hal::`:

    hal::Host   mapping (private/shared/persistent), files, clocks, entropy, topology, libraries
    hal::net    byte streams between nodes
    hal::sync   Mutex, LockGuard, Thread, sleep
    hal::*      (to come) console, block storage, interrupts/timers, accelerator runtime

Nothing above the HAL may know which implementation is underneath.

**Route A (decided):** the core keeps the standard containers. The C++
runtime is made freestanding against AOS primitives; the containers are not
rewritten to make the dependency graph look prettier. The rule that makes
this safe:

> The core may depend on C++ language/runtime facilities only when those
> facilities are satisfied by the AOS freestanding runtime. The core may not
> acquire an OS dependency merely because the standard library implementation
> happens to require one.

So the permitted chain is core → `std::unordered_map` → AOS freestanding C++
runtime → AOS allocator/primitives → HAL → hardware, never core → libstdc++ →
Linux. The 41-symbol surface is the frozen baseline; shrinking it is an
optimisation for after the kernel boots without Linux, not before.

The core also may not name a HAL implementation: it calls `hal::make_host()`
and links whichever HAL the build selects.

## The freestanding allowance

The core is allowed to need exactly these from a runtime, because a
freestanding runtime can provide them without an operating system:

| class | symbols today | freestanding provision |
|---|---|---|
| memory/string primitives | `memcpy memmove memset memcmp memchr strlen strcmp` | ~100 lines of C |
| libm | `expf log2 sqrt` | small implementations (or hardware `sqrtss`, polynomial exp/log) |
| allocator | `operator new/delete` | a kernel heap over `hal::Host` mappings |
| C++ runtime | `__cxa_*` guards/exceptions, `_Unwind_Resume`, `__gxx_personality_v0`, `std::__throw_*`, red-black tree and hash helpers, `basic_string` | a libstdc++ built freestanding against the primitives above, or the kernel's own containers; exceptions may be compiled out (`-fno-exceptions`) once container throw paths are trapped |
| toolchain | `__stack_chk_fail`, `_GLOBAL_OFFSET_TABLE_` | trivial |

Everything else is a violation. `scripts/zero-host-audit.sh` lists the core's
complete external symbol surface grouped by provider; `--gate` fails on any
symbol outside the allowance and runs as the `zero-host.gate` test.

## Where the balance stands

    kernel core: 21 objects, 41 external symbols
    POSIX / host threads ........ none   (was: pthread_mutex_*, std::thread — moved behind hal::sync)
    iostream / locale ........... none   (was: std::stringstream in capability parsing — removed)
    libc parsing ................ none   (was: strtoul in entity relation parsing — removed)
    libc primitives ............. 7      (allowance)
    libm ........................ 3      (allowance)
    C++ runtime + toolchain ..... 31     (allowance)

Header audit (`scripts/host-audit.sh`): outside `src/hal/`, the only host
headers in the tree are the OpenCL type header in the accelerator device and
the POSIX process headers in the external-process intelligence adapter.

## What the host still does for the kernel (through the HAL)

| facility | Linux today | bare metal needs |
|---|---|---|
| memory mapping | `mmap`, `memfd_create`, file-backed `mmap` | page allocator + page tables; shared = same frames mapped twice; persistent = block device + a filesystem or a log-structured store |
| files/directories | POSIX I/O | a filesystem on `hal::block` (the kernel uses: write-file-atomically, append, read, list, mkdir, exists) |
| clocks | `clock_gettime` | HPET/TSC + RTC |
| entropy | `/dev/urandom` | RDRAND / jitter |
| topology | `sysconf`, sysfs | ACPI MADT/SRAT |
| threads/locks | pthread | a kernel scheduler with spinlocks/futex-like blocking |
| network | BSD sockets | NIC driver (virtio-net first) + a minimal TCP/IP, or a native fabric protocol over Ethernet |
| libraries | `dlopen` | static registration of drivers and minds (no dynamic loading on bare metal) |
| console | tty via init | UART 16550 + VGA text/framebuffer |
| accelerators | OpenCL ICD | an accelerator HAL: for virtio-gpu / vendor firmware interfaces, later |
| processes (adapters) | fork/exec/pipes | an external mind becomes a peer node over `hal::net` instead |

## Demolition order

1. **Contract + gate** — done. The core cannot regress without failing a test.
2. **Threading behind HAL** — done (`hal::sync`).
3. **Iostream, parsing, formatting out of the core** — iostream and `strtoul`
   done; `std::to_string`/`snprintf` remain inside libstdc++ (`basic_string`
   helpers) and move to a kernel formatter next.
4. **Allocator behind HAL** — route `operator new/delete` to a kernel heap that
   draws from `hal::Host::map`; on Linux this is a mapping-backed arena.
5. **Freestanding runtime** — done (`metal/runtime.cpp`, route A): the hosted
   libstdc++ headers and archive are used unchanged; the runtime provides the
   memory/string primitives, a size-class heap, `operator new/delete`, the
   standard library's throw points as kernel panics, the C++ ABI hooks, three
   libm functions and their kin, and stubs for the glibc conveniences
   libstdc++'s demangler and terminate handler were built against. Exceptions
   are compiled out of the core; the unwinder is not linked. Image: 816 KB.
6. **Bare-metal `hal::Host` for x86-64 under QEMU** — done (`metal/`):
   Multiboot2 entry (`boot.S`: 32→64-bit, identity-mapped first GiB, SSE on),
   UART console, PIT-calibrated TSC clock, RDRAND, heap from the Multiboot
   memory map, memory-resident file tree for the persistent root, single
   hardware thread (locks trivial, threads run inline). `metal/build.sh --run`
   boots it: kernel identity, one CPU device, matmul [58 64 139 154] and
   reduce 415, a channel round-trip, a cognitive cycle with the deterministic
   mind, checkpoint → terminate → restore with the same identity, state,
   tensor memory, capabilities and execution position intact. 15/15, 71 ms
   of uptime, 182 KiB heap peak. **Linux is now an implementation of the HAL,
   not a prerequisite of the OS.** Registered as the `metal.acceptance` test.
7. **Block storage** — done: `hal::BlockDevice` (`metal/virtio_blk.cpp`, a
   polled legacy-transport virtio-blk driver; `src/hal/linux_block.cpp` over a
   file for driving the same code on Linux) and the **AOS volume**
   (`src/hal/volume.cpp`): AOS's own log-structured store — checksummed
   appended records, replay on mount, a torn tail dropped cleanly, live set
   rewritten into the other half when a half fills, superblock flips. It
   presents the file API the kernel's HAL needs; the metal HAL routes files
   and persistent mappings through it once mounted, and `host volume format`
   claims a disk and carries the live session onto it. `metal/drive-storage.py`
   (the `metal.storage` test): format, store state, power off, boot again from
   the disk, same kernel identity and stored object. No filesystem borrowed
   from any host. `volume.check` drives format/replay/compaction/torn-tail on
   Linux.
8. **Network** — done. `metal/virtio_net.cpp` (a polled legacy-transport
   virtio-net driver) is a `hal::FrameDevice`; the **AOS fabric**
   (`src/hal/fabric.cpp`) runs reliable byte streams directly over Ethernet
   frames (ethertype 0x88B5) with no IP and no host stack, presenting
   `hal::Listener`/`hal::Connection` so the node layer is unchanged. Links are
   authenticated and encrypted (ChaCha20-Poly1305, RFC 8439, `src/node/secure.cpp`,
   vectors in `secure.check`); the node service has a single-threaded polled
   mode pumped from the console loop. `metal/drive-fabric.py` (the
   `metal.fabric` test) runs two bare-metal kernels on a shared virtual
   segment: one attaches the other over the fabric, runs a matmul on its
   device, holds memory on it, and publishes state into a channel living on it.
   No Linux on either side.
9. **Media** — done. Two forms: the ISO (`metal/build.sh`) boots straight to
   the command surface and can claim an attached disk; the **disk image**
   (`metal/mkdisk.sh` → `aios-metal-disk.img`, 256 MiB, mostly zeros) is a
   deployable bare-metal AOS: MBR, a FAT boot partition with GRUB for BIOS and
   UEFI plus the kernel, and an AOS partition (MBR type 0x7F) the kernel
   formats on first boot. `metal/drive-disk.py` (the `metal.disk.bios` and
   `metal.disk.uefi` tests) boots it, stores state, powers off, boots again:
   same identity, same state, on both firmwares. Convert with `qemu-img` for
   VMware, VirtualBox or Hyper-V. Linux is not on the disk at all.

Steps 1–3 and 5–9 are in the tree; step 4 (the allocator behind the HAL) is
satisfied on bare metal by the runtime's heap over the HAL's memory map, while
on Linux `operator new` still reaches malloc — a tidy-up, not a dependency
the kernel can see. **AOS-010 is complete: the kernel boots, runs, stores,
deploys and networks with no Linux underneath.** What remains is optimisation
and breadth (more NIC/disk drivers, SMP, a smaller symbol surface), not the
architectural line.
