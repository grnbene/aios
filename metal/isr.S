/* Interrupt entry points. Every vector lands here, saves the machine's whole
 * register state as a frame the C++ handler can read AND REPLACE, and returns
 * through that frame — which is what makes preemption possible: the handler can
 * hand back a different process's frame and iretq resumes that one instead.
 *
 * Ten of the first thirty-two vectors are pushed by the CPU with an error code
 * and the rest are not, so the stubs that need to push a zero do, and every
 * frame below this line has the same shape. */
.section .text
.code64

.macro ISR_NOERR num
.globl isr_\num
isr_\num:
  pushq $0
  pushq $\num
  jmp isr_common
.endm

.macro ISR_ERR num
.globl isr_\num
isr_\num:
  pushq $\num
  jmp isr_common
.endm

  ISR_NOERR 0
  ISR_NOERR 1
  ISR_NOERR 2
  ISR_NOERR 3
  ISR_NOERR 4
  ISR_NOERR 5
  ISR_NOERR 6
  ISR_NOERR 7
  ISR_ERR  8
  ISR_NOERR 9
  ISR_ERR  10
  ISR_ERR  11
  ISR_ERR  12
  ISR_ERR  13
  ISR_ERR  14
  ISR_NOERR 15
  ISR_NOERR 16
  ISR_ERR  17
  ISR_NOERR 18
  ISR_NOERR 19
  ISR_NOERR 20
  ISR_ERR  21
  ISR_NOERR 22
  ISR_NOERR 23
  ISR_NOERR 24
  ISR_NOERR 25
  ISR_NOERR 26
  ISR_NOERR 27
  ISR_NOERR 28
  ISR_ERR  29
  ISR_ERR  30
  ISR_NOERR 31
  ISR_NOERR 32
  ISR_NOERR 33
  ISR_NOERR 34
  ISR_NOERR 35
  ISR_NOERR 36
  ISR_NOERR 37
  ISR_NOERR 38
  ISR_NOERR 39
  ISR_NOERR 40
  ISR_NOERR 41
  ISR_NOERR 42
  ISR_NOERR 43
  ISR_NOERR 44
  ISR_NOERR 45
  ISR_NOERR 46
  ISR_NOERR 47
/* The local APIC's spurious vector. It is not one of the 0-47 range and owes no
   acknowledgement, but it must land somewhere that is present in the IDT. */
.globl isr_255
isr_255:
  pushq $0
  pushq $255
  jmp isr_common

isr_common:
  pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx
  pushq %rsi; pushq %rdi; pushq %rbp
  pushq %r8;  pushq %r9;  pushq %r10; pushq %r11
  pushq %r12; pushq %r13; pushq %r14; pushq %r15
  cld
  /* Came from ring 3? Then the kernel's GS base is the shadow one. */
  testb $3, 18*8(%rsp)
  jz 1f
  swapgs
1:
  /* The floating-point register file belongs to whatever was interrupted, and
     the kernel is about to use those same registers for its own arithmetic. It
     is saved here, at the boundary, and not by the scheduler — by then it would
     already be the kernel's numbers being saved. */
  movq %gs:24, %rax
  testq %rax, %rax
  jz 5f
  fxsave (%rax)
5:
  movq %rsp, %rdi
  andq $-16, %rsp            /* the C++ handler is compiled expecting this */
  call metal_trap
  movq %rax, %rsp            /* the frame to resume, which need not be the one we took */
  /* And restored for whatever is being resumed, which may be a different task
     entirely — gs still points at the kernel's per-CPU block here. */
  movq %gs:24, %rax
  testq %rax, %rax
  jz 6f
  fxrstor (%rax)
6:
  testb $3, 18*8(%rsp)
  jz 2f
  swapgs
2:
  popq %r15; popq %r14; popq %r13; popq %r12
  popq %r11; popq %r10; popq %r9;  popq %r8
  popq %rbp; popq %rdi; popq %rsi
  popq %rdx; popq %rcx; popq %rbx; popq %rax
  addq $16, %rsp             /* vector and error code */
  iretq

/* The syscall instruction does not switch stacks: it lands here on the user
 * stack with the return address in rcx and the user rflags in r11. The kernel
 * stack for this CPU is where swapgs points, and the frame built here is the
 * SAME shape a trap builds, so one handler serves both and a syscall can also
 * be a scheduling point. */
.globl syscall_entry
syscall_entry:
  swapgs
  movq %rsp, %gs:16          /* remember the user stack */
  movq %gs:8, %rsp           /* the kernel stack for this CPU */
  /* Build an iretq-shaped frame by hand: ss, rsp, rflags, cs, rip. */
  pushq $0x23                /* user SS */
  pushq %gs:16
  pushq %r11                 /* user rflags, as syscall saved them */
  pushq $0x2B                /* user CS */
  pushq %rcx                 /* the instruction after syscall */
  pushq $0                   /* no error code */
  pushq $256                 /* the vector this machine calls a system call */
  pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx
  pushq %rsi; pushq %rdi; pushq %rbp
  pushq %r8;  pushq %r9;  pushq %r10; pushq %r11
  pushq %r12; pushq %r13; pushq %r14; pushq %r15
  cld
  movq %gs:24, %rax
  testq %rax, %rax
  jz 7f
  fxsave (%rax)
7:
  movq %rsp, %rdi
  andq $-16, %rsp
  call metal_trap
  movq %rax, %rsp
  movq %gs:24, %rax
  testq %rax, %rax
  jz 8f
  fxrstor (%rax)
8:
  popq %r15; popq %r14; popq %r13; popq %r12
  popq %r11; popq %r10; popq %r9;  popq %r8
  popq %rbp; popq %rdi; popq %rsi
  popq %rdx; popq %rcx; popq %rbx; popq %rax
  addq $16, %rsp
  /* The frame that comes back need not be the one that came in: a blocking
     system call can hand the CPU to the console, which is ring 0 and must NOT
     have gs swapped out from under it. Decide from the frame being resumed,
     exactly as an interrupt return does. */
  testb $3, 8(%rsp)
  jz 4f
  swapgs
4:
  iretq                      /* iretq, not sysret: it restores ss and rsp as well */

/* Enter a frame that was never taken from anywhere — a process being run for
 * the first time. */
.globl metal_frame_enter
metal_frame_enter:
  movq %rdi, %rsp
  testb $3, 18*8(%rsp)
  jz 3f
  swapgs
3:
  popq %r15; popq %r14; popq %r13; popq %r12
  popq %r11; popq %r10; popq %r9;  popq %r8
  popq %rbp; popq %rdi; popq %rsi
  popq %rdx; popq %rcx; popq %rbx; popq %rax
  addq $16, %rsp
  iretq

/* The console saves itself here before it enters the scheduler, and a process
 * that ends with nothing left to run returns to that exact point — from inside
 * an interrupt, on a different stack. */
.globl metal_ctx_save
metal_ctx_save:
  movq (%rsp), %rax
  movq %rax, 0(%rdi)
  leaq 8(%rsp), %rax
  movq %rax, 8(%rdi)
  movq %rbp, 16(%rdi)
  movq %rbx, 24(%rdi)
  movq %r12, 32(%rdi)
  movq %r13, 40(%rdi)
  movq %r14, 48(%rdi)
  movq %r15, 56(%rdi)
  xorl %eax, %eax
  ret

.globl metal_ctx_restore
metal_ctx_restore:
  movq 8(%rdi), %rsp
  movq 16(%rdi), %rbp
  movq 24(%rdi), %rbx
  movq 32(%rdi), %r12
  movq 40(%rdi), %r13
  movq 48(%rdi), %r14
  movq 56(%rdi), %r15
  movq 0(%rdi), %rcx
  movl $1, %eax
  sti
  jmp *%rcx

.section .rodata
.globl isr_stubs
isr_stubs:
  .quad isr_0
  .quad isr_1
  .quad isr_2
  .quad isr_3
  .quad isr_4
  .quad isr_5
  .quad isr_6
  .quad isr_7
  .quad isr_8
  .quad isr_9
  .quad isr_10
  .quad isr_11
  .quad isr_12
  .quad isr_13
  .quad isr_14
  .quad isr_15
  .quad isr_16
  .quad isr_17
  .quad isr_18
  .quad isr_19
  .quad isr_20
  .quad isr_21
  .quad isr_22
  .quad isr_23
  .quad isr_24
  .quad isr_25
  .quad isr_26
  .quad isr_27
  .quad isr_28
  .quad isr_29
  .quad isr_30
  .quad isr_31
  .quad isr_32
  .quad isr_33
  .quad isr_34
  .quad isr_35
  .quad isr_36
  .quad isr_37
  .quad isr_38
  .quad isr_39
  .quad isr_40
  .quad isr_41
  .quad isr_42
  .quad isr_43
  .quad isr_44
  .quad isr_45
  .quad isr_46
  .quad isr_47
  .quad isr_255

.section .note.GNU-stack,"",@progbits
