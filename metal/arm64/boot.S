/* AArch64 bare-metal entry for AIOS (metal/arm64/boot.S)
 *
 * QEMU's -kernel loads this ELF at the physical address the linker script
 * names and enters at _start with x0 = the flattened device tree. The same
 * entry is handed to PSCI CPU_ON for the secondary cores, which is why the
 * exception-level drop and the stack selection are written to be re-entered
 * by a core that is not core 0.
 */

/* 65536 (64KB) was tight enough to be a real suspect for the `desktop`
 * freeze: dash's own frame loop -> net_poll() -> dispatch_shell_command()
 * -> compositor_run() -> HardwareCompositor::render_desktop() (16,000
 * lines of nested drawing calls, compositor.cpp) -> net_poll() again
 * (added so the console stays alive while the desktop runs) -> dispatch
 * again. No panic text and no console reply matches a silent stack
 * overflow better than anything else tried (a corrupted return address
 * does not reliably reach metal_panic()). 8x costs ~4MB total across
 * MAX_CPUS cores - nothing against 944MB DRAM. Unverified by a stack-depth
 * measurement (no UART on this board, and crashlog.cpp's SD-write attempt
 * turned out not to create a host-discoverable file at all - fat32.cpp's
 * write_file() has no directory-entry/cluster-chain logic, just a fixed
 * physical sector - so this is the strongest remaining hypothesis, applied
 * directly, not a confirmed root cause. */
.equ STACK_SIZE, 524288
.equ MAX_CPUS,   8

/* SPSR value that returns to EL1h with all of DAIF masked. */
.equ SPSR_EL1H,  0x3C5

/* SCTLR_EL1 with the MMU, the data cache and the instruction cache OFF, and
 * the RES1 bits the architecture requires set. Anything else is a guess about
 * what the firmware left behind. */
.equ SCTLR_EL1_RESET_LO, 0x0800
.equ SCTLR_EL1_RESET_HI, 0x30D0

.section .text.boot, "ax"
.global _start
.type _start, %function

_start:
    /* Linux AArch64 flat-image header, so this same file can also be handed
     * to a bootloader that speaks that protocol rather than to -kernel. */
    b       real_start              /* 0x00 */
    .long   0                       /* 0x04 */
    .quad   0x80000                 /* 0x08 text_offset */
    .quad   __image_size            /* 0x10 image_size */
    .quad   0x8                     /* 0x18 flags: 4K pages, LE */
    .quad   0                       /* 0x20 */
    .quad   0                       /* 0x28 */
    .quad   0                       /* 0x30 */
    .long   0x644D5241              /* 0x38 magic "ARM\x64" */
    .long   0                       /* 0x3C */

real_start:
    msr     daifset, #0xF           /* no interrupts until ready */
    mov     x19, x0                 /* the DTB pointer has to survive the drop */

    /* Park secondary cores immediately: only Core 0 runs early boot */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, 1f
park_core:
    wfe
    b       park_core
1:
    mov     x20, xzr                /* core index: 0 here; spin_entry sets its own */
    b       el_detect

/* Where a secondary core lands when smp_start() releases it from the
 * firmware's spin table (Pi 3: no PSCI - cores 1-3 wait on 0xe0/0xe8/0xf0
 * for an address, then jump to it). Same exception-level drop and setup
 * as core 0, then its own stack and arm64_secondary_main(core). */
.global spin_entry
spin_entry:
    msr     daifset, #0xF
    mrs     x20, mpidr_el1
    and     x20, x20, #0xFF
    /* Join the cluster's coherency before any cache or MMU is on: Cortex-A53
     * CPUECTLR_EL1.SMPEN (bit 6). The firmware sets it for core 0 only; a
     * core without it kept its writes in its own cache - core 0 never saw
     * it come online, and atomics between cores were not atomics. Written
     * here, at EL2, where the armstub leaves parked cores. */
    mrs     x0, S3_1_C15_C2_1
    orr     x0, x0, #(1 << 6)
    msr     S3_1_C15_C2_1, x0
    isb

el_detect:

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #3
    b.eq    from_el3
    cmp     x0, #2
    b.eq    from_el2
    b       in_el1

from_el3:
    mrs     x0, scr_el3
    orr     x0, x0, #(1 << 10)      /* RW: EL2/EL1 are AArch64 */
    orr     x0, x0, #(1 << 0)       /* NS */
    msr     scr_el3, x0
    mov     x0, #SPSR_EL1H
    msr     spsr_el3, x0
    adr     x0, in_el1
    msr     elr_el3, x0
    eret

from_el2:
    mov     x0, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x0

    /* Without EL1PCTEN|EL1PCEN the generic timer reads we do at EL1 trap to a
     * hypervisor that is not there. CNTVOFF has to be zeroed by hand too: it
     * is architecturally UNKNOWN out of reset. */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    mov     x0, #0x33FF             /* CPTR_EL2: do not trap FP/SIMD */
    msr     cptr_el2, x0
    msr     hstr_el2, xzr

    mov     x0, #SPSR_EL1H
    msr     spsr_el2, x0
    adr     x0, in_el1
    msr     elr_el2, x0
    eret

in_el1:
    /* Put SCTLR_EL1 in a state we chose rather than one we inherited. */
    mov     x0, #SCTLR_EL1_RESET_LO
    movk    x0, #SCTLR_EL1_RESET_HI, lsl #16
    msr     sctlr_el1, x0
    isb

    /* FP and NEON at EL1 and EL0. The trap path saves q0-q31, and the compute
     * kernels are NEON, so this cannot wait for userland. */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    ldr     x0, =arm64_vector_table
    msr     vbar_el1, x0
    isb

    /* A secondary: stack slot core+1 (slot 0 is core 0's), then its main. */
    cbz     x20, core0_stack
    add     x0, x20, #1
    ldr     x1, =boot_stacks
    mov     x2, #STACK_SIZE
    madd    x1, x0, x2, x1
    mov     sp, x1
    b       secondary_entry
core0_stack:
    /* Set up stack for Core 0 */
    ldr     x0, =boot_stacks
    add     x0, x0, #STACK_SIZE
    mov     sp, x0

    /* Core 0 only: .bss is not zeroed by anyone else. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:
    mov     x0, x19
    bl      arm64_main
4:  wfe
    b       4b

secondary_entry:
    mov     x0, x20
    bl      arm64_secondary_main
5:  wfe
    b       5b

.size _start, . - _start
.ltorg

.section .bss
.align 16
.global boot_stacks
boot_stacks:
    .space STACK_SIZE * (MAX_CPUS + 1)
