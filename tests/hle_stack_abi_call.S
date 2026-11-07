.text
.globl _probe_horizon_call
.p2align 2
_probe_horizon_call:
    sub sp, sp, #64
    stp x29, x30, [sp, #32]
    mov x29, sp
    mov x16, x0
    mov x0, #1
    mov x1, #2
    mov x2, #3
    mov x3, #4
    mov x4, #5
    mov x5, #6
    mov x6, #7
    mov x7, #8
    mov x9, #9
    str x9, [sp]
    mov x9, #10
    str x9, [sp, #8]
    blr x16
    ldp x29, x30, [sp, #32]
    add sp, sp, #64
    ret
