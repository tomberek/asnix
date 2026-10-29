#include "asnix/ir.h"

    .text

    .globl asnix_op_lazy_thunk_body
    .type asnix_op_lazy_thunk_body, @function
asnix_op_lazy_thunk_body:
    movl  ASNIX_IROP_OFF_B(%rsi), %ecx
    movq  (%r13, %rcx, 8), %rdx
    pushq %rsi
    pushq %rdi
    movq  %rdx, %rdi
    call  asnix_compile_lazy_thunk_body
    popq  %rdi
    popq  %rsi
    movq  %rax, %rcx
    movq  ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq  ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq  (%rsi), %rax
    jmp   *%rax
    .size asnix_op_lazy_thunk_body, . - asnix_op_lazy_thunk_body

