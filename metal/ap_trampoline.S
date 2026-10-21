/* Application-processor trampoline, copied to physical 0x8000 and started by a
 * SIPI (vector 0x08). Real mode -> protected -> long mode, then into C. The BSP
 * patches the data slots (page-table root, this AP's stack, the C entry) before
 * each SIPI. Assembled flat; absolute references use TRAMP (0x8000). */
.set TRAMP, 0x8000
.code16
.text
.globl ap_trampoline_start
ap_trampoline_start:
  jmp ap_code
.org 0x08
ap_pml4:  .long 0
.org 0x10
ap_stack: .quad 0
.org 0x18
ap_entry: .quad 0
.org 0x20
ap_flag:  .long 0
.org 0x40
ap_gdt:
  .quad 0x0000000000000000
  .quad 0x00CF9A000000FFFF   /* 0x08 32-bit code */
  .quad 0x00CF92000000FFFF   /* 0x10 32-bit data */
  .quad 0x00AF9A000000FFFF   /* 0x18 64-bit code */
.org 0x60
ap_gdtr:
  .word 4*8 - 1
  .long ap_gdt + TRAMP
.org 0x80
ap_code:
  cli
  xorw %ax, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  lgdtl ap_gdtr + TRAMP
  movl %cr0, %eax
  orl $1, %eax
  movl %eax, %cr0
  ljmpl $0x08, $(ap_pm32 + TRAMP)
.code32
ap_pm32:
  movw $0x10, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movw %ax, %fs
  movw %ax, %gs
  movl (ap_pml4 + TRAMP), %eax
  movl %eax, %cr3
  movl %cr4, %eax
  orl $0x20, %eax           /* PAE */
  movl %eax, %cr4
  movl $0xC0000080, %ecx    /* EFER.LME */
  rdmsr
  orl $0x100, %eax
  wrmsr
  movl %cr0, %eax
  orl $0x80000000, %eax     /* PG */
  movl %eax, %cr0
  ljmpl $0x18, $(ap_lm64 + TRAMP)
.code64
ap_lm64:
  movq $(ap_stack + TRAMP), %rax
  movq (%rax), %rsp
  /* enable SSE, as the BSP did */
  movq %cr0, %rax
  andq $0xFFFFFFFFFFFFFFFB, %rax
  orq $0x2, %rax
  movq %rax, %cr0
  movq %cr4, %rax
  orq $0x600, %rax
  movq %rax, %cr4
  movq $(ap_entry + TRAMP), %rax
  movq (%rax), %rax
  call *%rax
1:
  hlt
  jmp 1b
.globl ap_trampoline_end
ap_trampoline_end:
.section .note.GNU-stack,"",@progbits
