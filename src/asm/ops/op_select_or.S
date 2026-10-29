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

    .globl asnix_op_select_or
    .type asnix_op_select_or, @function
asnix_op_select_or:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lselect_or_not_found

    movq %rax, %r9
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8
    testq %r8, %r8
    jns .Lselect_or_resolved
    pushq %rsi
    pushq %rdi
    subq $16, %rsp
    movq %r9, %rdi
    movl ASNIX_IROP_OFF_B(%rsi), %esi
    movq %rsp, %rdx
    call asnix_bshared_attrs_probe
    movq (%rsp), %r9
    addq $16, %rsp
    popq %rdi
    popq %rsi
    testl %eax, %eax
    jz .Lselect_or_not_found
    movq %r9, %rax
    jmp .Lselect_or_resolve
.Lselect_or_resolved:

    movl ASNIX_IROP_OFF_B(%rsi), %edx
    xorq %rcx, %rcx
.Lselect_or_loop:
    cmpq %r8, %rcx
    jge .Lselect_or_not_found
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d
    cmpq %rdx, %r11
    je .Lselect_or_found
    jl .Lselect_or_go_right
    movq %r9, %r8
    jmp .Lselect_or_loop
.Lselect_or_go_right:
    leaq 1(%r9), %rcx
    jmp .Lselect_or_loop
.Lselect_or_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lselect_or_resolve:
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lselect_or_check_cell

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lselect_or_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lselect_or_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lselect_or_reraise

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

.Lselect_or_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lselect_or_push

.Lselect_or_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_or_push
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_or_check_lazyapp

    movq $ASNIX_CELL_CHASE_LIMIT, %rdx
.Lselect_or_cell_loop:
    movq ASNIX_CELL_OFF_TARGET(%r8), %r10
    movq (%r10), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_or_resolve
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_or_resolve
    decq %rdx
    jnz .Lselect_or_cell_loop
    jmp .Lselect_or_blackhole

.Lselect_or_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %r9
    je .Lselect_or_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %r9
    je .Lselect_or_lazyapp
    jmp .Lselect_or_check_thunkmap
.Lselect_or_lazyapp:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_lazyapp
    popq %rdi
    popq %rsi
    jmp .Lselect_or_resolve

.Lselect_or_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %r9
    jne .Lselect_or_push
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_thunkmap
    popq %rdi
    popq %rsi
    jmp .Lselect_or_resolve
.Lselect_or_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_or_not_found:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_or_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lselect_or_reraise:
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_select_or, . - asnix_op_select_or

