    .bss
    .align 8
    .globl asnix_valstack_limit
    .type asnix_valstack_limit, @object
asnix_valstack_limit:
    .quad 0
    .size asnix_valstack_limit, 8

    .align 8
    .globl asnix_retsp_limit
    .type asnix_retsp_limit, @object
asnix_retsp_limit:
    .quad 0
    .size asnix_retsp_limit, 8

    .align 8
    .globl asnix_inprogress_head
    .type asnix_inprogress_head, @object
asnix_inprogress_head:
    .quad 0
    .size asnix_inprogress_head, 8

    .section .rodata
.Lmsg_valstack_overflow:
    .ascii "asnix: value stack overflow (too many live pushed values), aborting\n"
    .equ ASNIX_MSG_VALSTACK_OVERFLOW_LEN, . - .Lmsg_valstack_overflow
.Lmsg_retsp_overflow:
    .ascii "asnix: call/force stack overflow (too much nested recursion), aborting\n"
    .equ ASNIX_MSG_RETSP_OVERFLOW_LEN, . - .Lmsg_retsp_overflow

    .text

    .globl asnix_valstack_overflow
    .type asnix_valstack_overflow, @function
asnix_valstack_overflow:
    leaq .Lmsg_valstack_overflow(%rip), %rdi
    movq $ASNIX_MSG_VALSTACK_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_valstack_overflow, . - asnix_valstack_overflow

    .globl asnix_retsp_overflow
    .type asnix_retsp_overflow, @function
asnix_retsp_overflow:
    leaq .Lmsg_retsp_overflow(%rip), %rdi
    movq $ASNIX_MSG_RETSP_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_retsp_overflow, . - asnix_retsp_overflow

