#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_thunk
    .type asnix_op_mk_thunk, @function
asnix_op_mk_thunk:
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq (%r13, %rcx, 8), %rbp

    pushq %rsi
    pushq %rdi

    movq $ASNIX_THUNK_SIZE, %rdi
    call asnix_arena_alloc
    incq asnix_stats_thunk(%rip)
    movq %rbp, ASNIX_THUNK_OFF_STATE(%rax)

    popq %rdi
    movq %rdi, ASNIX_THUNK_OFF_ENV(%rax)
    popq %rsi

    movq %rax, %rdx
    orq $ASNIX_TAG_THUNK, %rdx
    ASNIX_CHECK_VALSTACK %rcx
    movq %rdx, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_thunk, . - asnix_op_mk_thunk

