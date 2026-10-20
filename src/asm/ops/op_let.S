#include "asnix/env.h"
#include "asnix/ir.h"

    .text

    .globl asnix_op_let
    .type asnix_op_let, @function
asnix_op_let:                           // a = nslots; binds the next `nslots` pushed values
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = nslots (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    leaq 8(, %rbx, 8), %rdi             // size = 8 + nslots*8
    call asnix_arena_alloc
    movq %r15, ASNIX_ENV_OFF_UP(%rax)

    movq %rbx, %rcx                     // rcx = remaining slots to bind, counting down
.Lop_let_bind:
    testq %rcx, %rcx
    jz .Lop_let_done
    decq %rcx
    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    jmp .Lop_let_bind
.Lop_let_done:
    movq %rax, %rdi                     // ENV = new frame
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_let, . - asnix_op_let
