#include "asnix/env.h"
#include "asnix/force_scratch.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .bss
    .align 8
    .globl asnix_apply_depth
asnix_apply_depth:
    .quad 0

    .equ ASNIX_APPLY_VALSTACK_BYTES, 262144
    .equ ASNIX_APPLY_RETSTACK_BYTES, 65536

    .equ ASNIX_APPLY_MAX_PRIMOP_ARGC, 8
    .equ ASNIX_APPLY_PRIMOP_SCRATCH_BYTES, 104

    .section .rodata
.Lmsg_apply_type:
    .ascii "asnix: apply target is not a function, aborting\n"
    .equ ASNIX_MSG_APPLY_TYPE_LEN, . - .Lmsg_apply_type
.Lmsg_apply_arity:
    .ascii "asnix: apply argument count does not match arity, aborting\n"
    .equ ASNIX_MSG_APPLY_ARITY_LEN, . - .Lmsg_apply_arity
.Lmsg_apply_too_many:
    .ascii "asnix: apply: too many arguments to a primop, aborting\n"
    .equ ASNIX_MSG_APPLY_TOO_MANY_LEN, . - .Lmsg_apply_too_many

    .data
    .align 8
.Lapply_sentinel_ir:
    .quad asnix_apply_epilogue
    .quad 0
    .quad 0

    .text

    .globl asnix_apply
    .type asnix_apply, @function
asnix_apply:
    movq %rdi, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_CLOSURE, %rcx
    je .Lapply_is_closure
    cmpq $ASNIX_TAG_PRIMOP, %rcx
    je .Lapply_is_primop
    cmpq $ASNIX_TAG_ATTRS, %rcx
    je .Lapply_is_attrs
    jmp .Lapply_type_error

.Lapply_is_attrs:
    cmpq $1, %rdx
    jne .Lapply_type_error
    movq (%rsi), %rsi
    jmp asnix_bshared_call_functor

.Lapply_is_primop:
    andq $-8, %rax
    movq ASNIX_PRIMOP_OFF_ARITY(%rax), %rcx
    cmpq %rcx, %rdx
    jne .Lapply_arity_error
    cmpq $ASNIX_APPLY_MAX_PRIMOP_ARGC, %rdx
    ja .Lapply_too_many_args

    movq ASNIX_PRIMOP_OFF_FN(%rax), %rcx
    movq ASNIX_PRIMOP_OFF_NOFORCE(%rax), %r8

    subq $ASNIX_APPLY_PRIMOP_SCRATCH_BYTES, %rsp
    movq %rcx, 0(%rsp)
    movq %rdx, 8(%rsp)
    movq %rsi, 16(%rsp)
    movq $0, 24(%rsp)
    movq %rdi, 32(%rsp)

    cmpq $0, %r8
    jne .Lapply_primop_skip_force
.Lapply_primop_force_loop:
    movq 24(%rsp), %rcx
    movq 8(%rsp), %rdx
    cmpq %rdx, %rcx
    jge .Lapply_primop_force_done
    movq 16(%rsp), %rsi
    movq (%rsi, %rcx, 8), %rdi
    call asnix_force_for_print
    movq 24(%rsp), %rcx
    movq %rax, 40(%rsp, %rcx, 8)
    incq %rcx
    movq %rcx, 24(%rsp)
    jmp .Lapply_primop_force_loop
.Lapply_primop_force_done:
    leaq 40(%rsp), %rsi
    jmp .Lapply_primop_call
.Lapply_primop_skip_force:
    movq 16(%rsp), %rsi
.Lapply_primop_call:
    movq %rsi, %rdi
    movq 8(%rsp), %rsi
    movq 0(%rsp), %rax
    call *%rax
    movq %rax, %rdi
    call asnix_force_for_print
    addq $ASNIX_APPLY_PRIMOP_SCRATCH_BYTES, %rsp
    ret

.Lapply_is_closure:
    andq $-8, %rax
    movq ASNIX_CLOSURE_OFF_ARITY(%rax), %rcx
    cmpq %rcx, %rdx
    jne .Lapply_arity_error

    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp

    movq %rax, %rbx
    movq %rsi, %rbp
    movq %rdx, %r15

    movq asnix_valstack_limit(%rip), %rax
    pushq %rax
    movq asnix_retsp_limit(%rip), %rax
    pushq %rax

    movq asnix_apply_depth(%rip), %rdi
    call asnix_apply_scratch_valstack
    movq %rax, %r12
    leaq ASNIX_APPLY_VALSTACK_BYTES(%r12), %rcx
    movq %rcx, asnix_valstack_limit(%rip)

    movq asnix_apply_depth(%rip), %rdi
    call asnix_apply_scratch_retstack
    movq %rax, %r14
    leaq ASNIX_APPLY_RETSTACK_BYTES(%r14), %rcx
    movq %rcx, asnix_retsp_limit(%rip)

    incq asnix_apply_depth(%rip)

    movq %r15, %rcx
    leaq ASNIX_ENV_OFF_VALUES(, %rcx, 8), %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %r15, asnix_stats_env_elements(%rip)
    movq ASNIX_CLOSURE_OFF_UP(%rbx), %rcx
    testq %rcx, %rcx
    jz .Lapply_up_null
    subq asnix_env_arena_base(%rip), %rcx
.Lapply_up_null:
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    xorq %rcx, %rcx
.Lapply_copy_args:
    cmpq %r15, %rcx
    jge .Lapply_copy_done
    movq (%rbp, %rcx, 8), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    incq %rcx
    jmp .Lapply_copy_args
.Lapply_copy_done:

    ASNIX_CHECK_RETSP %rcx
    leaq .Lapply_sentinel_ir(%rip), %rcx
    movq %rcx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq $0, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq $0, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq $0, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq %rax, %rdi
    movq ASNIX_CLOSURE_OFF_PROG(%rbx), %rcx
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq (%rsi), %rax
    jmp *%rax

asnix_apply_epilogue:
    movq -8(%r12), %rax
    decq asnix_apply_depth(%rip)
    popq %rcx
    movq %rcx, asnix_retsp_limit(%rip)
    popq %rcx
    movq %rcx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret

.Lapply_type_error:
    leaq .Lmsg_apply_type(%rip), %rdi
    movq $ASNIX_MSG_APPLY_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lapply_arity_error:
    leaq .Lmsg_apply_arity(%rip), %rdi
    movq $ASNIX_MSG_APPLY_ARITY_LEN, %rsi
    jmp asnix_fatal
.Lapply_too_many_args:
    leaq .Lmsg_apply_too_many(%rip), %rdi
    movq $ASNIX_MSG_APPLY_TOO_MANY_LEN, %rsi
    jmp asnix_fatal
    .size asnix_apply, . - asnix_apply

