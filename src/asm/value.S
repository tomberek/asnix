#include "asnix/value.h"

    .section .rodata
.Lmsg_make_int_overflow:
    .ascii "asnix: integer literal/result out of asnix's representable range, aborting\n"
    .equ ASNIX_MSG_MAKE_INT_OVERFLOW_LEN, . - .Lmsg_make_int_overflow

    .text

    .globl asnix_make_int
    .type asnix_make_int, @function
asnix_make_int:
    .cfi_startproc
    movq %rdi, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lmake_int_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lmake_int_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq  $ASNIX_TAG_INT, %rax
    ret
.Lmake_int_overflow:
    leaq .Lmsg_make_int_overflow(%rip), %rdi
    movq $ASNIX_MSG_MAKE_INT_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .cfi_endproc
    .size asnix_make_int, . - asnix_make_int

    .globl asnix_int_value
    .type asnix_int_value, @function
asnix_int_value:
    .cfi_startproc
    movq %rdi, %rax
    sarq $ASNIX_TAG_BITS, %rax
    ret
    .cfi_endproc
    .size asnix_int_value, . - asnix_int_value

    .globl asnix_make_imm
    .type asnix_make_imm, @function
asnix_make_imm:
    .cfi_startproc
    movl %edi, %eax
    salq $ASNIX_TAG_BITS, %rax
    orq  $ASNIX_TAG_IMM, %rax
    ret
    .cfi_endproc
    .size asnix_make_imm, . - asnix_make_imm

    .globl asnix_imm_subtag
    .type asnix_imm_subtag, @function
asnix_imm_subtag:
    .cfi_startproc
    movq %rdi, %rax
    shrq $ASNIX_TAG_BITS, %rax
    ret
    .cfi_endproc
    .size asnix_imm_subtag, . - asnix_imm_subtag

    .globl asnix_tag_of
    .type asnix_tag_of, @function
asnix_tag_of:
    .cfi_startproc
    movq %rdi, %rax
    andq $ASNIX_TAG_MASK, %rax
    ret
    .cfi_endproc
    .size asnix_tag_of, . - asnix_tag_of

    .globl asnix_is_thunk
    .type asnix_is_thunk, @function
asnix_is_thunk:
    .cfi_startproc
    movq %rdi, %rax
    andq $ASNIX_TAG_MASK, %rax
    cmpq $ASNIX_TAG_THUNK, %rax
    sete %al
    movzbl %al, %eax
    ret
    .cfi_endproc
    .size asnix_is_thunk, . - asnix_is_thunk

