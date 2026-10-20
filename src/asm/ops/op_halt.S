// OP_HALT is the one genuine `ret`: it unwinds back to asnix_eval's C
// caller, restoring the callee-saved registers the dispatch core claimed
// at entry (see src/asm/dispatch.S).
    .text

    .globl asnix_op_halt
    .type asnix_op_halt, @function
asnix_op_halt:
    subq $8, %r12
    movq (%r12), %rax                   // final result
    addq $8, %rsp                       // undo dispatch.S's alignment padding
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret
    .size asnix_op_halt, . - asnix_op_halt
