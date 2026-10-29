#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_call_arity:
    .ascii "asnix: call argument count does not match arity, aborting\n"
    .equ ASNIX_MSG_CALL_ARITY_LEN, . - .Lmsg_call_arity

    .text

    .globl asnix_op_call
    .type asnix_op_call, @function
asnix_op_call:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx

    movq %rbx, %rax
    incq %rax
    shlq $3, %rax
    movq %r12, %rcx
    subq %rax, %rcx
    movq (%rcx), %rdx

    movq %rdx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_CLOSURE, %r8
    je .Lop_call_is_closure
    cmpq $ASNIX_TAG_PRIMOP, %r8
    je .Lop_call_is_primop
    cmpq $ASNIX_TAG_ATTRS, %r8
    je .Lop_call_is_attrs
    jmp .Lop_call_type_error

.Lop_call_is_attrs:
    cmpq $1, %rbx
    jne .Lop_call_type_error
    movq %rcx, %rbx
    movq 8(%rbx), %r9

    pushq %rsi
    pushq %rdi
    movq %rdx, %rdi
    movq %r9, %rsi
    call asnix_bshared_call_functor
    popq %rdi
    popq %rsi

    movq %rbx, %r12
    ASNIX_CHECK_VALSTACK %r8
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_is_primop:
    andq $-8, %rdx
    movq ASNIX_PRIMOP_OFF_ARITY(%rdx), %r8
    cmpq %r8, %rbx
    jne .Lop_call_arity_error

    movq ASNIX_PRIMOP_OFF_FN(%rdx), %rbp
    movq %rcx, %r15

    pushq %rsi
    pushq %rdi

    cmpq $0, ASNIX_PRIMOP_OFF_NOFORCE(%rdx)
    jne .Lop_call_force_args_done
    subq $16, %rsp
    xorq %r9, %r9
.Lop_call_force_args:
    cmpq %rbx, %r9
    jge .Lop_call_force_args_done_pop
    movq 8(%r15, %r9, 8), %rdi
    movq %r9, (%rsp)
    call asnix_force_for_print
    movq (%rsp), %r9
    movq %rax, 8(%r15, %r9, 8)
    incq %r9
    jmp .Lop_call_force_args
.Lop_call_force_args_done_pop:
    addq $16, %rsp
.Lop_call_force_args_done:

    leaq 8(%r15), %rdi
    movq %rbx, %rsi
    call *%rbp
    movq %rax, %rdi
    call asnix_force_for_print
    popq %rdi
    popq %rsi

    movq %r15, %r12
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_is_closure:
    andq $-8, %rdx

    movq ASNIX_CLOSURE_OFF_ARITY(%rdx), %r8
    cmpq %r8, %rbx
    jne .Lop_call_arity_error

    movq %rdx, %rbp
    movq %rcx, %r15

    pushq %rsi
    pushq %rdi

    movq %rbx, %rcx
    leaq ASNIX_ENV_OFF_VALUES(, %rcx, 8), %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbx, asnix_stats_env_elements(%rip)
    movq ASNIX_CLOSURE_OFF_UP(%rbp), %rcx
    testq %rcx, %rcx
    jz .Lop_call_up_null
    subq asnix_env_arena_base(%rip), %rcx
.Lop_call_up_null:
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    xorq %rcx, %rcx
.Lop_call_copy_args:
    cmpq %rbx, %rcx
    jge .Lop_call_copy_done
    movq 8(%r15, %rcx, 8), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    incq %rcx
    jmp .Lop_call_copy_args
.Lop_call_copy_done:

    movq %r15, %r12

    popq %rdx
    popq %rcx
    addq $ASNIX_IROP_SIZE, %rcx

    ASNIX_CHECK_RETSP %r8
    movq %rcx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdx, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq $0, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq %rax, %rdi
    movq ASNIX_CLOSURE_OFF_PROG(%rbp), %rcx
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_type_error:
    movq %rdx, %rdi
    call asnix_bshared_call_type_fatal
.Lop_call_arity_error:
    leaq .Lmsg_call_arity(%rip), %rdi
    movq $ASNIX_MSG_CALL_ARITY_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_call, . - asnix_op_call

    .globl asnix_op_ret
    .type asnix_op_ret, @function
asnix_op_ret:
    subq $ASNIX_RETFRAME_SIZE, %r14
    movq ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14), %rcx
    testq %rcx, %rcx
    jz .Lret_no_memo
    movq ASNIX_THUNK_OFF_ENV(%rcx), %rax
    movq %rax, asnix_inprogress_head(%rip)
    movq -8(%r12), %rdx
    movq %rdx, ASNIX_THUNK_OFF_ENV(%rcx)
    movq $ASNIX_THUNK_STATE_FORCED, ASNIX_THUNK_OFF_STATE(%rcx)
.Lret_no_memo:
    movq ASNIX_RETFRAME_OFF_IP(%r14), %rsi
    movq ASNIX_RETFRAME_OFF_ENV(%r14), %rdi
    movq ASNIX_RETFRAME_OFF_POOL(%r14), %r13
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ret, . - asnix_op_ret

