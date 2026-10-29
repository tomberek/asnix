#include "asnix/force_scratch.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .bss
    .align 8
    .globl asnix_force_depth
asnix_force_depth:
    .quad 0

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed

    .data
    .align 8
.Lforce_sentinel_ir:
    .quad asnix_force_epilogue
    .quad 0
    .quad 0

    .text

    .globl asnix_force_for_print
    .type asnix_force_for_print, @function
asnix_force_for_print:
.Lforce_top:
    movq %rdi, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lforce_done

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %rdx
    andq $ASNIX_TAG_MASK, %rdx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rdx
    je .Lforce_cached
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rdx
    je .Lforce_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %rdx
    je .Lforce_reraise

    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp
    movq %rax, %rbx
    movq %rcx, %rbp

    movq asnix_valstack_limit(%rip), %rdx
    pushq %rdx
    movq asnix_retsp_limit(%rip), %rdx
    pushq %rdx

    movq asnix_force_depth(%rip), %rdi
    call asnix_force_scratch_valstack
    movq %rax, %r12
    leaq ASNIX_FORCE_VALSTACK_BYTES(%r12), %rdx
    movq %rdx, asnix_valstack_limit(%rip)

    movq asnix_force_depth(%rip), %rdi
    call asnix_force_scratch_retstack
    movq %rax, %r14
    leaq ASNIX_FORCE_RETSTACK_BYTES(%r14), %rdx
    movq %rdx, asnix_retsp_limit(%rip)

    incq asnix_force_depth(%rip)
    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rbx)

    leaq .Lforce_sentinel_ir(%rip), %rax
    ASNIX_CHECK_RETSP %rcx
    movq %rax, ASNIX_RETFRAME_OFF_IP(%r14)
    movq $0, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq $0, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rbx, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_IRPROG_OFF_POOL(%rbp), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rbp), %rsi
    movq ASNIX_THUNK_OFF_ENV(%rbx), %rdi
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rbx)
    movq %rbx, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

asnix_force_epilogue:
    movq -8(%r12), %rax
    decq asnix_force_depth(%rip)
    popq %rdx
    movq %rdx, asnix_retsp_limit(%rip)
    popq %rdx
    movq %rdx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    movq %rax, %rdi
    jmp .Lforce_done

.Lforce_cached:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
.Lforce_done:
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lforce_check_cell
    movq %rax, %rdi
    jmp .Lforce_top

.Lforce_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lforce_ret
    movq %rax, %rdx
    andq $-8, %rdx
    shlq $16, %rdx
    shrq $16, %rdx
    movq ASNIX_CELL_OFF_MARKER(%rdx), %rcx
    cmpq $ASNIX_CELL_MARKER, %rcx
    jne .Lforce_check_lazyapp

    movq $ASNIX_CELL_CHASE_LIMIT, %r8
.Lforce_cell_loop:
    movq ASNIX_CELL_OFF_TARGET(%rdx), %r9
    movq (%r9), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lforce_cell_done
    movq %rax, %rdx
    andq $-8, %rdx
    shlq $16, %rdx
    shrq $16, %rdx
    movq ASNIX_CELL_OFF_MARKER(%rdx), %rcx
    cmpq $ASNIX_CELL_MARKER, %rcx
    jne .Lforce_cell_done
    decq %r8
    jnz .Lforce_cell_loop
    jmp .Lforce_blackhole
.Lforce_cell_done:
    movq %rax, %rdi
    jmp .Lforce_top

.Lforce_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %rcx
    je .Lforce_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %rcx
    je .Lforce_lazyapp
    jmp .Lforce_check_thunkmap
.Lforce_lazyapp:
    movq %rax, %rdi
    subq $8, %rsp
    call asnix_resolve_lazyapp
    addq $8, %rsp
    movq %rax, %rdi
    jmp .Lforce_top

.Lforce_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %rcx
    jne .Lforce_ret
    movq %rax, %rdi
    subq $8, %rsp
    call asnix_resolve_thunkmap
    addq $8, %rsp
    movq %rax, %rdi
    jmp .Lforce_top
.Lforce_ret:
    ret

.Lforce_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lforce_reraise:
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_force_for_print, . - asnix_force_for_print

