#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_insert_dynamic_attr
    .type asnix_op_insert_dynamic_attr, @function
asnix_op_insert_dynamic_attr:
    subq $24, %r12
    movq 16(%r12), %rcx
    movq 8(%r12), %rdx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    call asnix_attrs_insert_dynamic
    popq %rdi
    popq %rsi

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_insert_dynamic_attr, . - asnix_op_insert_dynamic_attr

