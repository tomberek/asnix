#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_select_dynamic
    .type asnix_op_select_dynamic, @function
asnix_op_select_dynamic:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_select_dynamic
    popq %rdi
    popq %rsi

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_select_dynamic, . - asnix_op_select_dynamic

