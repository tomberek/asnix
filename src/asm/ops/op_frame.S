#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"

    .text

    .globl asnix_op_frame_alloc
    .type asnix_op_frame_alloc, @function
asnix_op_frame_alloc:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movq %rsi, %rbp
    movq %rdi, %r15

    leaq ASNIX_ENV_OFF_VALUES(, %rbx, 8), %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbx, asnix_stats_env_elements(%rip)
    movq %r15, %rcx
    subq asnix_env_arena_base(%rip), %rcx
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    movq %rax, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_alloc, . - asnix_op_frame_alloc

    .globl asnix_op_frame_set
    .type asnix_op_frame_set, @function
asnix_op_frame_set:
    movl ASNIX_IROP_OFF_A(%rsi), %edx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8)

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_set, . - asnix_op_frame_set

    .globl asnix_op_frame_get
    .type asnix_op_frame_get, @function
asnix_op_frame_get:
    movl ASNIX_IROP_OFF_A(%rsi), %edx
    movq ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8), %rax

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_get, . - asnix_op_frame_get

    .globl asnix_op_frame_exit
    .type asnix_op_frame_exit, @function
asnix_op_frame_exit:
    movl ASNIX_ENV_OFF_UP(%rdi), %edi
    addq asnix_env_arena_base(%rip), %rdi

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_exit, . - asnix_op_frame_exit

