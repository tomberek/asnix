#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_select_type:
    .ascii "asnix: '.' selection target is not an attrset, aborting\n"
    .equ ASNIX_MSG_SELECT_TYPE_LEN, . - .Lmsg_select_type
.Lmsg_select_missing:
    .ascii "asnix: attribute missing, aborting\n"
    .equ ASNIX_MSG_SELECT_MISSING_LEN, . - .Lmsg_select_missing
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole

    .text

    .globl asnix_op_select
    .type asnix_op_select, @function
asnix_op_select:                        // a = symbol id; pops an attrset, pushes the matching value
    subq $8, %r12
    movq (%r12), %rax                   // rax = attrs tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lselect_type_error
    andq $-8, %rax                      // rax = real Attrs*

    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = target symbol id
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lselect_loop:
    cmpq %r8, %rcx
    jge .Lselect_missing
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    shlq $4, %r10                       // r10 = mid * ASNIX_ATTR_SIZE
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11
    cmpq %rdx, %r11
    je .Lselect_found
    jl .Lselect_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lselect_loop
.Lselect_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lselect_loop
.Lselect_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

    // M6: force if the selected entry holds an unforced thunk (laziness
    // -- attrset binding values are deferred the same way `let` bindings
    // are; see op_mk_thunk.S / compiler.c).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lselect_push

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lselect_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lselect_blackhole

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rax), %rsi
    movq ASNIX_THUNK_OFF_POOL(%rax), %r13
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
.Lselect_push:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_type_error:
    leaq .Lmsg_select_type(%rip), %rdi
    movq $ASNIX_MSG_SELECT_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lselect_missing:
    leaq .Lmsg_select_missing(%rip), %rdi
    movq $ASNIX_MSG_SELECT_MISSING_LEN, %rsi
    jmp asnix_fatal
.Lselect_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_select, . - asnix_op_select
