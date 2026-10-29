#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_select_dynamic_or
    .type asnix_op_select_dynamic_or, @function
asnix_op_select_dynamic_or:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    subq $16, %rsp
    movq %rax, %rdi
    movq %rcx, %rsi
    movq %rsp, %rdx
    call asnix_bshared_select_dynamic_or
    movq (%rsp), %rcx
    addq $16, %rsp
    popq %rdi
    popq %rsi

    testl %eax, %eax
    jz .Lsdo_miss

    ASNIX_CHECK_VALSTACK %rdx
    movq %rcx, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lsdo_miss:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_select_dynamic_or, . - asnix_op_select_dynamic_or

