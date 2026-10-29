#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_rec_overrides_merge
    .type asnix_op_rec_overrides_merge, @function
asnix_op_rec_overrides_merge:
    movl ASNIX_IROP_OFF_A(%rsi), %eax
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    leaq (%r13, %rcx, 8), %rcx

    pushq %rsi
    pushq %rdi
    movq %rax, %rsi
    movq %rcx, %rdx
    call asnix_bshared_rec_overrides_merge
    popq %rdi
    popq %rsi

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_rec_overrides_merge, . - asnix_op_rec_overrides_merge

