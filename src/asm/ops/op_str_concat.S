#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_str_concat
    .type asnix_op_str_concat, @function
asnix_op_str_concat:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movq %rsi, %rbp
    movq %rdi, %r15

    movq %rbx, %rdx
    shlq $3, %rdx
    movq %r12, %rdi
    subq %rdx, %rdi
    movq %rbx, %rsi
    call asnix_concat_strings

    movq %rbx, %rdx
    shlq $3, %rdx
    subq %rdx, %r12
    movq %rax, (%r12)
    addq $8, %r12

    movq %r15, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_str_concat, . - asnix_op_str_concat

