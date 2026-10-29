    .text

    .globl asnix_op_halt
    .type asnix_op_halt, @function
asnix_op_halt:
    subq $8, %r12
    movq (%r12), %rax
    decq asnix_eval_depth(%rip)
    popq %rdx
    movq %rdx, asnix_retsp_limit(%rip)
    popq %rdx
    movq %rdx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret
    .size asnix_op_halt, . - asnix_op_halt

