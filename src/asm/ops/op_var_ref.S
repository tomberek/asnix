#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed

    .text

    .globl asnix_op_var_ref
    .type asnix_op_var_ref, @function
asnix_op_var_ref:
    movq %rdi, %rcx
    movl ASNIX_IROP_OFF_A(%rsi), %edx
.Lvar_ref_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_done
    movl ASNIX_ENV_OFF_UP(%rcx), %ecx
    addq asnix_env_arena_base(%rip), %rcx
    andq $-2, %rcx
    decq %rdx
    jmp .Lvar_ref_walk
.Lvar_ref_done:
    movl ASNIX_IROP_OFF_B(%rsi), %edx
    movq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rax

.Lvar_ref_resolve:
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lvar_ref_check_cell

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lvar_ref_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lvar_ref_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lvar_ref_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    ASNIX_CHECK_RETSP %r8
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rax)
    movq %rax, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lvar_ref_push

.Lvar_ref_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lvar_ref_push
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movslq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lvar_ref_check_lazyapp

    movq $ASNIX_CELL_CHASE_LIMIT, %rdx
.Lvar_ref_cell_loop:
    movl ASNIX_CELL_OFF_TARGET(%r8), %r10d
    addq asnix_env_arena_base(%rip), %r10
    movq (%r10), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lvar_ref_resolve
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movslq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lvar_ref_resolve
    decq %rdx
    jnz .Lvar_ref_cell_loop
    jmp .Lvar_ref_blackhole

.Lvar_ref_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %r9
    je .Lvar_ref_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %r9
    je .Lvar_ref_lazyapp
    jmp .Lvar_ref_check_thunkmap
.Lvar_ref_lazyapp:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_lazyapp
    popq %rdi
    popq %rsi
    jmp .Lvar_ref_resolve

.Lvar_ref_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %r9
    jne .Lvar_ref_push
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_thunkmap
    popq %rdi
    popq %rsi
    jmp .Lvar_ref_resolve
.Lvar_ref_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lvar_ref_reraise:
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_var_ref, . - asnix_op_var_ref

