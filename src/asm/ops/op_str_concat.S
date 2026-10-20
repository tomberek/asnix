#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_str_concat
    .type asnix_op_str_concat, @function
asnix_op_str_concat:                    // a = count; pops `count` values (reverse push order,
                                         // same convention as OP_MK_LIST), coerces each to a
                                         // string and concatenates them in source order.
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = count (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save ENV (ditto)

    movq %rbx, %rdx
    shlq $3, %rdx                       // rdx = count * 8
    movq %r12, %rdi
    subq %rdx, %rdi                     // rdi = base ptr = VALSTACK top - count*8 (asnix_concat_strings' `parts`)
    movq %rbx, %rsi                     // rsi = count
    call asnix_concat_strings           // rax = result Value (already tagged ASNIX_TAG_PTR)

    movq %rbx, %rdx
    shlq $3, %rdx
    subq %rdx, %r12                     // pop `count` slots
    movq %rax, (%r12)                   // push result
    addq $8, %r12

    movq %r15, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_str_concat, . - asnix_op_str_concat
