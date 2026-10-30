#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_with_type:
    .ascii "asnix: `with' value is not an attrset, aborting\n"
    .equ ASNIX_MSG_WITH_TYPE_LEN, . - .Lmsg_with_type
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed
.Lmsg_undefined_var:
    .ascii "asnix: undefined variable, aborting\n"
    .equ ASNIX_MSG_UNDEFINED_VAR_LEN, . - .Lmsg_undefined_var

    .text

    .globl asnix_op_with
    .type asnix_op_with, @function
asnix_op_with:
    movq %rsi, %rbp
    movq %rdi, %r15

    movq $ASNIX_ENV_OFF_VALUES+8, %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    incq asnix_stats_env_elements(%rip)

    movq %r15, %rcx
    orq $ASNIX_ENV_WITH_TAG, %rcx
    subq asnix_env_arena_base(%rip), %rcx
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax)

    movq %rax, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_with, . - asnix_op_with

    .globl asnix_op_with_try
    .type asnix_op_with_try, @function
asnix_op_with_try:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lwith_try_type_error

    movq %rax, %r9
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8
    testq %r8, %r8
    jns .Lwith_try_resolved
    pushq %rsi
    pushq %rdi
    movq %r9, %rdi
    call asnix_bshared_attrs_resolve
    movq %rax, %r9
    popq %rdi
    popq %rsi
    movq %r9, %rax
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8
.Lwith_try_resolved:

    movl ASNIX_IROP_OFF_B(%rsi), %edx
    xorq %rcx, %rcx
.Lwith_try_loop:
    cmpq %r8, %rcx
    jge .Lwith_try_not_found
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d
    cmpq %rdx, %r11
    je .Lwith_try_found
    jl .Lwith_try_go_right
    movq %r9, %r8
    jmp .Lwith_try_loop
.Lwith_try_go_right:
    leaq 1(%r9), %rcx
    jmp .Lwith_try_loop
.Lwith_try_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lwith_try_resolve:
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lwith_try_check_cell

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lwith_try_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lwith_try_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lwith_try_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
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

.Lwith_try_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lwith_try_push

.Lwith_try_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lwith_try_push
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movslq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lwith_try_check_lazyapp

    movq $ASNIX_CELL_CHASE_LIMIT, %rdx
.Lwith_try_cell_loop:
    movl ASNIX_CELL_OFF_TARGET(%r8), %r10d
    addq asnix_env_arena_base(%rip), %r10
    movq (%r10), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lwith_try_resolve
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movslq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lwith_try_resolve
    decq %rdx
    jnz .Lwith_try_cell_loop
    jmp .Lwith_try_blackhole

.Lwith_try_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %r9
    je .Lwith_try_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %r9
    je .Lwith_try_lazyapp
    jmp .Lwith_try_check_thunkmap
.Lwith_try_lazyapp:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_lazyapp
    popq %rdi
    popq %rsi
    jmp .Lwith_try_resolve

.Lwith_try_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %r9
    jne .Lwith_try_push
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_thunkmap
    popq %rdi
    popq %rsi
    jmp .Lwith_try_resolve
.Lwith_try_push:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_not_found:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_type_error:
    leaq .Lmsg_with_type(%rip), %rdi
    movq $ASNIX_MSG_WITH_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lwith_try_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
.Lwith_try_reraise:
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_with_try, . - asnix_op_with_try

    .globl asnix_op_with_miss
    .type asnix_op_with_miss, @function
asnix_op_with_miss:
    leaq .Lmsg_undefined_var(%rip), %rdi
    movq $ASNIX_MSG_UNDEFINED_VAR_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_with_miss, . - asnix_op_with_miss

    .globl asnix_op_with_exit
    .type asnix_op_with_exit, @function
asnix_op_with_exit:
    movl ASNIX_ENV_OFF_UP(%rdi), %edi
    addq asnix_env_arena_base(%rip), %rdi
    andq $-2, %rdi

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_with_exit, . - asnix_op_with_exit

