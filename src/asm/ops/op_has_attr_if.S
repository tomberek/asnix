#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_has_attr_if
    .type asnix_op_has_attr_if, @function
asnix_op_has_attr_if:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lhas_attr_if_false

    movq %rax, %r9
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8
    testq %r8, %r8
    jns .Lhas_attr_if_resolved
    pushq %rsi
    pushq %rdi
    subq $16, %rsp
    movq %r9, %rdi
    movl ASNIX_IROP_OFF_B(%rsi), %esi
    movq %rsp, %rdx
    call asnix_bshared_attrs_probe
    addq $16, %rsp
    popq %rdi
    popq %rsi
    testl %eax, %eax
    jnz .Lhas_attr_if_true
    jmp .Lhas_attr_if_false
.Lhas_attr_if_resolved:

    movl ASNIX_IROP_OFF_B(%rsi), %edx
    xorq %rcx, %rcx
.Lhas_attr_if_loop:
    cmpq %r8, %rcx
    jge .Lhas_attr_if_false
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d
    cmpq %rdx, %r11
    je .Lhas_attr_if_true
    jl .Lhas_attr_if_go_right
    movq %r9, %r8
    jmp .Lhas_attr_if_loop
.Lhas_attr_if_go_right:
    leaq 1(%r9), %rcx
    jmp .Lhas_attr_if_loop

.Lhas_attr_if_true:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lhas_attr_if_false:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr_if, . - asnix_op_has_attr_if

