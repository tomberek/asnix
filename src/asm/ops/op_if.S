#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .section .rodata
.Lmsg_if_type:
    .ascii "asnix: `if' condition is not a boolean, aborting\n"
    .equ ASNIX_MSG_IF_TYPE_LEN, . - .Lmsg_if_type

    .text

    .globl asnix_op_if
    .type asnix_op_if, @function
asnix_op_if:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_IMM, %rcx
    jne .Lif_type_error
    shrq $ASNIX_TAG_BITS, %rax
    cmpq $ASNIX_IMM_FALSE, %rax
    je .Lif_false
    cmpq $ASNIX_IMM_TRUE, %rax
    jne .Lif_type_error

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lif_false:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lif_type_error:
    leaq .Lmsg_if_type(%rip), %rdi
    movq $ASNIX_MSG_IF_TYPE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_if, . - asnix_op_if

    .globl asnix_op_jmp
    .type asnix_op_jmp, @function
asnix_op_jmp:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_jmp, . - asnix_op_jmp

