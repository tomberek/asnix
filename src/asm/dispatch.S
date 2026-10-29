#include "asnix/force_scratch.h"
#include "asnix/ir.h"

    .equ ASNIX_VALSTACK_BYTES, 1048576
    .equ ASNIX_RETSTACK_BYTES, 1048576

    .bss
    .align 8
    .globl asnix_eval_depth
asnix_eval_depth:
    .quad 0

    .text

    .globl asnix_eval
    .type asnix_eval, @function
asnix_eval:
    .cfi_startproc
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp

    movq %rdi, %rbx
    movq %rsi, %rbp

    movq asnix_valstack_limit(%rip), %rcx
    pushq %rcx
    movq asnix_retsp_limit(%rip), %rcx
    pushq %rcx

    movq asnix_eval_depth(%rip), %rdi
    call asnix_eval_scratch_valstack
    movq %rax, %r12
    leaq ASNIX_VALSTACK_BYTES(%r12), %rcx
    movq %rcx, asnix_valstack_limit(%rip)

    movq asnix_eval_depth(%rip), %rdi
    call asnix_eval_scratch_retstack
    movq %rax, %r14
    leaq ASNIX_RETSTACK_BYTES(%r14), %rcx
    movq %rcx, asnix_retsp_limit(%rip)

    incq asnix_eval_depth(%rip)

    movq %rbx, %rcx
    movq %rbp, %rdi
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    .cfi_endproc

    movq (%rsi), %rax
    jmp *%rax
    .size asnix_eval, . - asnix_eval

