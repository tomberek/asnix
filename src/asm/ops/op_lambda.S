#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_lambda
    .type asnix_op_lambda, @function
asnix_op_lambda:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq (%r13, %rcx, 8), %rbp

    pushq %rsi
    pushq %rdi

    movq $ASNIX_CLOSURE_SIZE, %rdi
    call asnix_arena_alloc
    incq asnix_stats_closure(%rip)
    movq %rbp, ASNIX_CLOSURE_OFF_PROG(%rax)
    movq %rbx, ASNIX_CLOSURE_OFF_ARITY(%rax)

    popq %rdi
    movq %rdi, ASNIX_CLOSURE_OFF_UP(%rax)
    popq %rsi

    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq 8(%r13, %rcx, 8), %rdx
    movq %rdx, ASNIX_CLOSURE_OFF_FORMALS(%rax)

    movq %rax, %rdx
    orq $ASNIX_TAG_CLOSURE, %rdx
    ASNIX_CHECK_VALSTACK %rcx
    movq %rdx, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_lambda, . - asnix_op_lambda

