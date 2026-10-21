/* AIOS bare-metal entry for x86-64. GRUB (Multiboot2) hands us a 32-bit
 * protected-mode machine; we identity-map the first 1 GiB with 2 MiB pages,
 * enter long mode, enable SSE, and call metal_start(magic, mbi). */
.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0
.set MB2_LEN,   mb2_end - mb2_start

.section .multiboot2, "a"
.align 8
mb2_start:
  .long MB2_MAGIC
  .long MB2_ARCH
  .long MB2_LEN
  .long -(MB2_MAGIC + MB2_ARCH + MB2_LEN)
  /* framebuffer tag (type 5, optional): ask the loader for a linear
     framebuffer so the console has a screen under UEFI, where there is no
     VGA text mode.
     
     The size here is what the LOADER is asked for, and the loader is the only
     thing in the boot path that still has UEFI boot services — so it is the
     only thing that can talk to a real GPU's GOP and set a mode on hardware
     this kernel has no driver for. Asking for 1024x768 is why an Intel or AMD
     machine came up at 1024x768: not a limit of the firmware, a limit of the
     request. Ask for 1920x1080; a loader that cannot manage it picks the
     closest mode it has, and metal_display_best_mode() then upgrades the
     Bochs-class adapters it CAN program itself. Any mode is rendered. */
  .short 5
  .short 1
  .long 20
  .long 1920
  .long 1080
  .long 32
  .long 0        /* pad to 8 */
  /* end tag */
  .short 0
  .short 0
  .long 8
mb2_end:

.section .bss
/* The stack sits BELOW the page tables so an overflow cannot silently rewrite
 * the mapping; 1 MiB costs nothing on disk. */
.align 4096
stack_bottom: .skip 1048576
stack_top:
.align 4096
pml4: .skip 4096
pdpt: .skip 4096

.section .data
.align 8
.globl mb_magic
.globl mb_info
mb_magic: .quad 0
mb_info:  .quad 0

.section .rodata
.align 16
gdt64:
  .quad 0
  .quad 0x00AF9A000000FFFF   /* 64-bit code */
  .quad 0x00CF92000000FFFF   /* data */
gdt64_ptr:
  .short gdt64_ptr - gdt64 - 1
  .quad gdt64

.section .text
.code32
.globl _start
_start:
  cli
  movl %eax, mb_magic
  movl %ebx, mb_info
  movl $stack_top, %esp

  /* zero .bss ourselves: the loader is not trusted to */
  movl $__bss_start, %edi
  movl $__kernel_end, %ecx
  subl %edi, %ecx
  shrl $2, %ecx
  xorl %eax, %eax
  cld
  rep stosl

  /* pml4[0] -> pdpt; pdpt[i] = i*1GiB | present|rw|huge, for the first 4 GiB
     (covers RAM and MMIO below 4 GiB: LAPIC 0xFEE00000, IOAPIC 0xFEC00000) */
  movl $pdpt, %eax
  orl  $0x3, %eax
  movl %eax, pml4
  movl $0, %ecx
  movl $pdpt, %edi
1:
  movl %ecx, %eax
  shll $30, %eax
  orl  $0x83, %eax
  movl %eax, (%edi)
  movl %ecx, %eax
  shrl $2, %eax
  movl %eax, 4(%edi)
  addl $8, %edi
  incl %ecx
  cmpl $4, %ecx
  jne 1b

  /* PAE + PGE */
  movl %cr4, %eax
  orl  $0xA0, %eax
  movl %eax, %cr4
  movl $pml4, %eax
  movl %eax, %cr3
  /* EFER.LME */
  movl $0xC0000080, %ecx
  rdmsr
  orl  $0x100, %eax
  wrmsr
  /* paging on, and clear EM / set MP for SSE */
  movl %cr0, %eax
  orl  $0x80000000, %eax
  andl $0xFFFFFFFB, %eax
  orl  $0x2, %eax
  movl %eax, %cr0
  lgdt gdt64_ptr
  ljmp $0x08, $long_mode

.code64
long_mode:
  movw $0x10, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movw $0, %ax
  movw %ax, %fs
  movw %ax, %gs
  movq $stack_top, %rsp
  /* SSE: OSFXSR | OSXMMEXCPT */
  movq %cr4, %rax
  orq  $0x600, %rax
  movq %rax, %cr4
  movl mb_magic, %edi
  movl mb_info, %esi
  call metal_start
2:
  hlt
  jmp 2b

.section .note.GNU-stack,"",@progbits
