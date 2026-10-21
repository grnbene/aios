/* AArch64 exception vectors for AIOS (metal/arm64/vectors.S)
 *
 * The architecture gives each of the sixteen vectors exactly 128 bytes. A full
 * register save does not fit in 128 bytes, so each slot is a four-instruction
 * stub that reserves the frame, stamps its own vector number and branches to
 * one shared save/dispatch/restore body. Writing the save inline in the slot
 * silently pushes every later vector past its architectural offset and the
 * table stops being a table.
 */

/* Must match struct TrapFrame in include/aios/metal/trap.hpp:
 *   x[31] @0  sp_el0 @248  elr_el1 @256  spsr_el1 @264
 *   esr_el1 @272  far_el1 @280  vector @288  q[32][2] @296..807
 * rounded up to a 16-byte stack boundary. */
.equ FRAME_SIZE,   816
.equ FRAME_SP_EL0, 248
.equ FRAME_ELR,    256
.equ FRAME_SPSR,   264
.equ FRAME_ESR,    272
.equ FRAME_FAR,    280
.equ FRAME_VEC,    288
.equ FRAME_Q,      296

.section .text, "ax"

.macro VECTOR_STUB id
.align 7
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\id
    b       trap_common
.endm

.align 11
.global arm64_vector_table
arm64_vector_table:
    /* Current EL, SP_EL0 */
    VECTOR_STUB 0
    VECTOR_STUB 1
    VECTOR_STUB 2
    VECTOR_STUB 3
    /* Current EL, SP_ELx */
    VECTOR_STUB 4
    VECTOR_STUB 5
    VECTOR_STUB 6
    VECTOR_STUB 7
    /* Lower EL, AArch64 */
    VECTOR_STUB 8
    VECTOR_STUB 9
    VECTOR_STUB 10
    VECTOR_STUB 11
    /* Lower EL, AArch32 */
    VECTOR_STUB 12
    VECTOR_STUB 13
    VECTOR_STUB 14
    VECTOR_STUB 15

.type trap_common, %function
trap_common:
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    str     x0, [sp, #FRAME_VEC]
    mrs     x1, sp_el0
    str     x1, [sp, #FRAME_SP_EL0]
    mrs     x1, elr_el1
    str     x1, [sp, #FRAME_ELR]
    mrs     x1, spsr_el1
    str     x1, [sp, #FRAME_SPSR]
    mrs     x1, esr_el1
    str     x1, [sp, #FRAME_ESR]
    mrs     x1, far_el1
    str     x1, [sp, #FRAME_FAR]

    add     x1, sp, #FRAME_Q
    stp     q0,  q1,  [x1, #(0 * 32)]
    stp     q2,  q3,  [x1, #(1 * 32)]
    stp     q4,  q5,  [x1, #(2 * 32)]
    stp     q6,  q7,  [x1, #(3 * 32)]
    stp     q8,  q9,  [x1, #(4 * 32)]
    stp     q10, q11, [x1, #(5 * 32)]
    stp     q12, q13, [x1, #(6 * 32)]
    stp     q14, q15, [x1, #(7 * 32)]
    stp     q16, q17, [x1, #(8 * 32)]
    stp     q18, q19, [x1, #(9 * 32)]
    stp     q20, q21, [x1, #(10 * 32)]
    stp     q22, q23, [x1, #(11 * 32)]
    stp     q24, q25, [x1, #(12 * 32)]
    stp     q26, q27, [x1, #(13 * 32)]
    stp     q28, q29, [x1, #(14 * 32)]
    stp     q30, q31, [x1, #(15 * 32)]

    mov     x0, sp
    bl      metal_trap
    /* metal_trap returns the frame to resume, which for a context switch is a
     * different frame on a different stack. */
    mov     sp, x0

.global arm64_frame_resume
.type arm64_frame_resume, %function
arm64_frame_resume:
    add     x1, sp, #FRAME_Q
    ldp     q0,  q1,  [x1, #(0 * 32)]
    ldp     q2,  q3,  [x1, #(1 * 32)]
    ldp     q4,  q5,  [x1, #(2 * 32)]
    ldp     q6,  q7,  [x1, #(3 * 32)]
    ldp     q8,  q9,  [x1, #(4 * 32)]
    ldp     q10, q11, [x1, #(5 * 32)]
    ldp     q12, q13, [x1, #(6 * 32)]
    ldp     q14, q15, [x1, #(7 * 32)]
    ldp     q16, q17, [x1, #(8 * 32)]
    ldp     q18, q19, [x1, #(9 * 32)]
    ldp     q20, q21, [x1, #(10 * 32)]
    ldp     q22, q23, [x1, #(11 * 32)]
    ldp     q24, q25, [x1, #(12 * 32)]
    ldp     q26, q27, [x1, #(13 * 32)]
    ldp     q28, q29, [x1, #(14 * 32)]
    ldp     q30, q31, [x1, #(15 * 32)]

    ldr     x0, [sp, #FRAME_SP_EL0]
    msr     sp_el0, x0
    ldr     x0, [sp, #FRAME_ELR]
    msr     elr_el1, x0
    ldr     x0, [sp, #FRAME_SPSR]
    msr     spsr_el1, x0

    ldr     x30,      [sp, #240]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8,  x9,  [sp, #64]
    ldp     x6,  x7,  [sp, #48]
    ldp     x4,  x5,  [sp, #32]
    ldp     x2,  x3,  [sp, #16]
    ldp     x0,  x1,  [sp, #0]

    add     sp, sp, #FRAME_SIZE
    eret

/* arm64_enter_frame(TrapFrame*) — drop into a frame that was built by hand
 * rather than taken from an exception. Used to run a process for the first
 * time; it never returns to its caller. */
.global arm64_enter_frame
.type arm64_enter_frame, %function
arm64_enter_frame:
    mov     sp, x0
    b       arm64_frame_resume

/* The scheduler's setjmp/longjmp pair. metal_ctx_save returns 0 the first
 * time and 1 when a later metal_ctx_restore lands on it. */
.global metal_ctx_save
.type metal_ctx_save, %function
metal_ctx_save:
    mov     x1, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    str     x1,       [x0, #96]
    mov     x0, #0
    ret

.global metal_ctx_restore
.type metal_ctx_restore, %function
metal_ctx_restore:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x1,       [x0, #96]
    mov     sp, x1
    mov     x0, #1
    ret
