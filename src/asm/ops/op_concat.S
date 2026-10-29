#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_concat
    .type asnix_op_concat, @function
asnix_op_concat:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_concat_lists
    popq %rdi
    popq %rsi

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_concat, . - asnix_op_concat

