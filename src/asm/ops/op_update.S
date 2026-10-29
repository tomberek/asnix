#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_update_type:
    .ascii "asnix: '//' operands must both be attrsets, aborting\n"
    .equ ASNIX_MSG_UPDATE_TYPE_LEN, . - .Lmsg_update_type

    .text

    .globl asnix_op_update
    .type asnix_op_update, @function
asnix_op_update:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_ATTRS, %r8
    jne .Lupdate_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_ATTRS, %r8
    jne .Lupdate_type_error

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_attrs_update
    popq %rdi
    popq %rsi

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lupdate_type_error:
    leaq .Lmsg_update_type(%rip), %rdi
    movq $ASNIX_MSG_UPDATE_TYPE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_update, . - asnix_op_update

