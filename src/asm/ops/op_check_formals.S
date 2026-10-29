#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_cf_type:
    .ascii "asnix: lambda pattern argument is not a set, aborting\n"
    .equ ASNIX_MSG_CF_TYPE_LEN, . - .Lmsg_cf_type

    .text

    .globl asnix_op_check_formals
    .type asnix_op_check_formals, @function
asnix_op_check_formals:
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lcf_type_error

    movq %rax, %r10
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %rbx
    testq %rbx, %rbx
    jns .Lcf_resolved
    pushq %rsi
    pushq %rdi
    movq %r10, %rdi
    call asnix_bshared_attrs_resolve
    movq %rax, %r10
    popq %rdi
    popq %rsi
    movq %r10, %rax
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %rbx
.Lcf_resolved:
    leaq ASNIX_ATTRS_OFF_ENTRIES(%rax), %rbp

    movl ASNIX_IROP_OFF_A(%rsi), %r8d
    movl ASNIX_IROP_OFF_B(%rsi), %r9d

    xorq %r10, %r10
.Lcf_req_loop:
    cmpq %r8, %r10
    jge .Lcf_req_done
    movq %r9, %r11
    addq $1, %r11
    addq %r10, %r11
    movq (%r13, %r11, 8), %r15

    xorq %rcx, %rcx
    movq %rbx, %rdx
.Lcf_req_bs:
    cmpq %rdx, %rcx
    jge .Lcf_missing_error
    leaq (%rcx, %rdx), %r11
    sarq $1, %r11
    movq %r11, %rax
    imulq $ASNIX_ATTR_SIZE, %rax
    movl ASNIX_ATTR_OFF_SYM(%rbp, %rax, 1), %eax
    cmpq %r15, %rax
    je .Lcf_req_found
    jl .Lcf_req_right
    movq %r11, %rdx
    jmp .Lcf_req_bs
.Lcf_req_right:
    leaq 1(%r11), %rcx
    jmp .Lcf_req_bs
.Lcf_req_found:
    incq %r10
    jmp .Lcf_req_loop
.Lcf_req_done:

    movq (%r13, %r9, 8), %rax
    cmpq $-1, %rax
    je .Lcf_done
    movq %rax, %r15
    movq %r9, %r10
    addq $1, %r10
    addq %r8, %r10

    xorq %r8, %r8
.Lcf_extra_loop:
    cmpq %rbx, %r8
    jge .Lcf_done
    movq %r8, %rax
    imulq $ASNIX_ATTR_SIZE, %rax
    movl ASNIX_ATTR_OFF_SYM(%rbp, %rax, 1), %r11d

    xorq %rcx, %rcx
    movq %r15, %rdx
.Lcf_extra_bs:
    cmpq %rdx, %rcx
    jge .Lcf_extra_error
    leaq (%rcx, %rdx), %rax
    sarq $1, %rax
    movq %r10, %r9
    addq %rax, %r9
    movq (%r13, %r9, 8), %r9
    cmpq %r11, %r9
    je .Lcf_extra_found
    jl .Lcf_extra_right
    movq %rax, %rdx
    jmp .Lcf_extra_bs
.Lcf_extra_right:
    leaq 1(%rax), %rcx
    jmp .Lcf_extra_bs
.Lcf_extra_found:
    incq %r8
    jmp .Lcf_extra_loop

.Lcf_done:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lcf_type_error:
    leaq .Lmsg_cf_type(%rip), %rdi
    movq $ASNIX_MSG_CF_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lcf_missing_error:
    movq %r15, %rdi
    call asnix_bshared_cf_missing_fatal
.Lcf_extra_error:
    movq %r11, %rdi
    call asnix_bshared_cf_extra_fatal
    .size asnix_op_check_formals, . - asnix_op_check_formals

