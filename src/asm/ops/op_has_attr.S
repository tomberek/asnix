#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

// `expr ? attr` / lambda-pattern default checks: never a fatal error, unlike
// op_select.S's OP_SELECT -- a non-attrset left-hand side simply means "no"
// (confirmed against real `nix-instantiate --eval '1 ? a'` => false), and a
// present-but-unforced entry is never forced: this is a membership test on
// symbol ids only, not a read of the value.

    .text

    .globl asnix_op_has_attr
    .type asnix_op_has_attr, @function
asnix_op_has_attr:                      // a = symbol id; pops a value, pushes TRUE/FALSE
    subq $8, %r12
    movq (%r12), %rax                   // rax = tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lhas_attr_false
    andq $-8, %rax                      // rax = Attrs*

    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = target symbol id
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lhas_attr_loop:
    cmpq %r8, %rcx
    jge .Lhas_attr_false
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    shlq $4, %r10                       // r10 = mid * ASNIX_ATTR_SIZE
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11
    cmpq %rdx, %r11
    je .Lhas_attr_true
    jl .Lhas_attr_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lhas_attr_loop
.Lhas_attr_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lhas_attr_loop

.Lhas_attr_true:
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jmp .Lhas_attr_push
.Lhas_attr_false:
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
.Lhas_attr_push:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr, . - asnix_op_has_attr
