#include "asnix/env.h"
#include "asnix/ir.h"

    .text

    .globl asnix_op_let
    .type asnix_op_let, @function
asnix_op_let:                           // a = nslots; binds the next `nslots` pushed values
    movl ASNIX_IROP_OFF_A(%rsi), %ebx   // rbx = nslots (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    leaq ASNIX_ENV_OFF_VALUES(, %rbx, 8), %rdi // size = 4 (up) + nslots*8
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbx, asnix_stats_env_elements(%rip)
    // M118: compress the current ENV (%r15) -- unconditional, see
    // op_frame.S's identical note (ENV is always live/non-NULL here).
    movq %r15, %rcx
    subq asnix_env_arena_base(%rip), %rcx
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

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
