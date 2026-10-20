// OP_HALT is the one genuine `ret`: it unwinds back to asnix_eval's C
// caller, restoring the callee-saved registers the dispatch core claimed
// at entry (see src/asm/dispatch.S).
    .text

    .globl asnix_op_halt
    .type asnix_op_halt, @function
asnix_op_halt:
    subq $8, %r12
    movq (%r12), %rax                   // final result
    // Restore the enclosing asnix_eval's (if any) VALSTACK/RETSP limits --
    // see dispatch.S's prologue for why this matters: without it, a nested
    // asnix_eval returning (e.g. `import`'s eval_file.c -> asnix_eval) would
    // leave the resumed outer invocation checking its pushes against this
    // call's now-defunct buffer instead of its own. Popped in reverse of
    // dispatch.S's push order (RETSP limit was pushed last).
    popq %rdx
    movq %rdx, asnix_retsp_limit(%rip)
    popq %rdx
    movq %rdx, asnix_valstack_limit(%rip)
    addq $8, %rsp                       // undo dispatch.S's alignment padding
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret
    .size asnix_op_halt, . - asnix_op_halt
