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
    .size asnix_op_select, . - asnix_op_select
