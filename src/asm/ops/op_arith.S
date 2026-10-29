#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_div_zero:
    .ascii "asnix: division by zero, aborting\n"
    .equ ASNIX_MSG_DIV_ZERO_LEN, . - .Lmsg_div_zero
.Lmsg_add_overflow:
    .ascii "asnix: integer overflow in '+', aborting\n"
    .equ ASNIX_MSG_ADD_OVERFLOW_LEN, . - .Lmsg_add_overflow
.Lmsg_sub_overflow:
    .ascii "asnix: integer overflow in '-', aborting\n"
    .equ ASNIX_MSG_SUB_OVERFLOW_LEN, . - .Lmsg_sub_overflow
.Lmsg_mul_overflow:
    .ascii "asnix: integer overflow in '*', aborting\n"
    .equ ASNIX_MSG_MUL_OVERFLOW_LEN, . - .Lmsg_mul_overflow
.Lmsg_div_overflow:
    .ascii "asnix: integer overflow in '/', aborting\n"
    .equ ASNIX_MSG_DIV_OVERFLOW_LEN, . - .Lmsg_div_overflow

    .text

    .globl asnix_op_add
    .type asnix_op_add, @function
asnix_op_add:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ladd_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ladd_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    addq %rcx, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Ladd_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Ladd_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Ladd_store

.Ladd_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_add_values
    popq %rdi
    popq %rsi

.Ladd_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Ladd_overflow:
    leaq .Lmsg_add_overflow(%rip), %rdi
    movq $ASNIX_MSG_ADD_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_add, . - asnix_op_add

    .globl asnix_op_sub
    .type asnix_op_sub, @function
asnix_op_sub:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lsub_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lsub_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    subq %rcx, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lsub_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lsub_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Lsub_store

.Lsub_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_sub_values
    popq %rdi
    popq %rsi

.Lsub_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lsub_overflow:
    leaq .Lmsg_sub_overflow(%rip), %rdi
    movq $ASNIX_MSG_SUB_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_sub, . - asnix_op_sub

    .globl asnix_op_mul
    .type asnix_op_mul, @function
asnix_op_mul:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    imulq %rcx, %rax
    jo .Lmul_overflow
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lmul_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lmul_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Lmul_store

.Lmul_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_mul_values
    popq %rdi
    popq %rsi

.Lmul_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lmul_overflow:
    leaq .Lmsg_mul_overflow(%rip), %rdi
    movq $ASNIX_MSG_MUL_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_mul, . - asnix_op_mul

    .globl asnix_op_div
    .type asnix_op_div, @function
asnix_op_div:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ldiv_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ldiv_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    testq %rcx, %rcx
    jz .Ldiv_zero_error
    cqto
    idivq %rcx
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Ldiv_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Ldiv_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Ldiv_store

.Ldiv_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_div_values
    popq %rdi
    popq %rsi

.Ldiv_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Ldiv_zero_error:
    leaq .Lmsg_div_zero(%rip), %rdi
    movq $ASNIX_MSG_DIV_ZERO_LEN, %rsi
    jmp asnix_fatal
.Ldiv_overflow:
    leaq .Lmsg_div_overflow(%rip), %rdi
    movq $ASNIX_MSG_DIV_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_div, . - asnix_op_div

