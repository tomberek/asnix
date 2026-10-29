#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_has_attr
    .type asnix_op_has_attr, @function
asnix_op_has_attr:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lhas_attr_false

    movq %rax, %r9
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8
    testq %r8, %r8
    jns .Lhas_attr_resolved
    pushq %rsi
    pushq %rdi
    subq $16, %rsp
    movq %r9, %rdi
    movl ASNIX_IROP_OFF_A(%rsi), %esi
    movq %rsp, %rdx
    call asnix_bshared_attrs_probe
    addq $16, %rsp
    popq %rdi
    popq %rsi
    testl %eax, %eax
    jnz .Lhas_attr_true
    jmp .Lhas_attr_false
.Lhas_attr_resolved:

    movl ASNIX_IROP_OFF_A(%rsi), %edx
    xorq %rcx, %rcx
.Lhas_attr_loop:
    cmpq %r8, %rcx
    jge .Lhas_attr_false
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d
    cmpq %rdx, %r11
    je .Lhas_attr_true
    jl .Lhas_attr_go_right
    movq %r9, %r8
    jmp .Lhas_attr_loop
.Lhas_attr_go_right:
    leaq 1(%r9), %rcx
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

