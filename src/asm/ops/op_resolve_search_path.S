#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_resolve_search_path
    .type asnix_op_resolve_search_path, @function
asnix_op_resolve_search_path:
    subq $8, %r12
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    call asnix_resolve_search_path_value
    popq %rdi
    popq %rsi

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_resolve_search_path, . - asnix_op_resolve_search_path

