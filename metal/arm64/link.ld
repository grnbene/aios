/* AArch64 bare-metal image layout for AIOS.
 *
 * The code is not position-independent, so it has to be LINKED where it is
 * loaded. The Raspberry Pi firmware loads kernel8.img at physical 0x80000
 * (its DRAM starts at 0). QEMU virt's DRAM starts at 0x40000000 and -kernel
 * loads an ELF at the ELF's own addresses (0x80000 there is flash, so a Pi
 * image booted on virt printed nothing) and a flat Image at RAM base +
 * text_offset. build-arm64.sh therefore links twice: the Pi image at the
 * default base, and aios-metal-arm64-virt.elf with --defsym KERNEL_BASE=0x40080000.
 */
OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = DEFINED(KERNEL_BASE) ? KERNEL_BASE : 0x80000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
    }

    .data : ALIGN(4096) {
        *(.data .data.* .got .got.plt .data.rel.ro .data.rel.ro.*)
    }

    /* Thread-local storage image. Nothing used to set up TLS on this kernel:
     * every thread_local (SqlDatabase's transaction state, the SQL pool's
     * worker flag, ...) read through an unset TPIDR_EL0 - the database pane
     * faulted on a garbage pointer the moment it opened. main.cpp copies this
     * image into a per-core block and points TPIDR_EL0 at it. */
    .tdata : ALIGN(16) {
        __tdata_start = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    }
    .tbss : ALIGN(16) {
        __tbss_start = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        __tbss_end = .;
    }

    /* The page tables are handed to the MMU by physical address and have to be
     * 4 KiB aligned; their own output section keeps that true however the
     * compiler decides to order the rest of .bss. */
    .pagetables (NOLOAD) : ALIGN(4096) {
        *(.pagetables)
    }

    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;
    __image_size = __kernel_end - __kernel_start;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
