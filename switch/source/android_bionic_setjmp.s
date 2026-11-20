// Matched AArch64 guest setjmp/longjmp providers for Morrow, not newlib aliases.
// Bionic's 32-word buffer is private. Both of these functions use the same
// layout: x30/sp at words 2..3; x28..x19 at 4..15; d14..d8 at 16..23.
// x18 is platform-reserved on Horizon and is deliberately not restored.

.text
.p2align 2
.global morrow_bionic_setjmp
.type morrow_bionic_setjmp, %function
morrow_bionic_setjmp:
    mov x9, sp
    stp x30, x9, [x0, #16]
    stp x28, x29, [x0, #32]
    stp x26, x27, [x0, #48]
    stp x24, x25, [x0, #64]
    stp x22, x23, [x0, #80]
    stp x20, x21, [x0, #96]
    stp xzr, x19, [x0, #112]
    stp d14, d15, [x0, #128]
    stp d12, d13, [x0, #144]
    stp d10, d11, [x0, #160]
    stp d8, d9, [x0, #176]
    // The helper records the native thread and validates saved fields.
    // Preserve the original LR across the helper's BL.
    stp x0, x30, [sp, #-16]!
    bl morrow_jump_seal
    ldp x9, x30, [sp], #16
    mov w0, wzr
    ret
.size morrow_bionic_setjmp, .-morrow_bionic_setjmp

.p2align 2
.global morrow_bionic_longjmp
.type morrow_bionic_longjmp, %function
morrow_bionic_longjmp:
    // Validate before touching SP or callee-saved registers. Keeping the
    // incoming arguments on the *current* stack also survives a C call.
    stp x0, x1, [sp, #-32]!
    str x30, [sp, #16]
    bl morrow_jump_require_valid
    ldp x9, x10, [sp], #32
    ldp d14, d15, [x9, #128]
    ldp d12, d13, [x9, #144]
    ldp d10, d11, [x9, #160]
    ldp d8, d9, [x9, #176]
    ldp x30, x11, [x9, #16]
    ldp x28, x29, [x9, #32]
    ldp x26, x27, [x9, #48]
    ldp x24, x25, [x9, #64]
    ldp x22, x23, [x9, #80]
    ldp x20, x21, [x9, #96]
    ldr x19, [x9, #120]
    mov sp, x11
    mov w0, w10
    cbnz w0, 1f
    mov w0, #1
1:
    ret
.size morrow_bionic_longjmp, .-morrow_bionic_longjmp

.section .note.GNU-stack,"",%progbits
