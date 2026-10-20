#include "asnix/env.h"
#include "asnix/ir.h"

// Lambda-patterns: OP_FRAME_NEW + OP_FRAME_BIND_SLOT split OP_LET's
// "allocate a frame, then fill its slots" into two separate ops so the
// compiler can construct each field's thunk (OP_MK_THUNK, unchanged)
// *between* them -- every such thunk captures the new frame (ENV is
// updated by OP_FRAME_NEW before any OP_MK_THUNK runs) as its own scope,
// before any of that frame's slots are actually bound. That gives pattern
// fields letrec-style mutual/forward visibility (confirmed against real
// Nix: `{a ? b+1, b ? 5}: a` sees `b` from `a`'s default even though `b` is
// declared after `a`) without touching OP_LET or plain `let`'s still-
// documented non-recursive scope limitation (see docs/architecture.md).

    .text

    .globl asnix_op_frame_new
    .type asnix_op_frame_new, @function
asnix_op_frame_new:                     // a = nslots; new Env frame, up = current ENV, becomes ENV
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = nslots (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    leaq 8(, %rbx, 8), %rdi             // size = 8 + nslots*8 (slots left unbound -- see op_frame_bind_slot below)
    call asnix_arena_alloc
    movq %r15, ASNIX_ENV_OFF_UP(%rax)

    movq %rax, %rdi                     // ENV = new frame (not restored to the old value: the point
                                         // of this op is to make the new frame current)
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_new, . - asnix_op_frame_new

    .globl asnix_op_frame_bind_slot
    .type asnix_op_frame_bind_slot, @function
asnix_op_frame_bind_slot:               // a = slot index; pops VALSTACK top into ENV->values[a]
    movq ASNIX_IROP_OFF_A(%rsi), %rcx
    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rdi, %rcx, 8)
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_bind_slot, . - asnix_op_frame_bind_slot
