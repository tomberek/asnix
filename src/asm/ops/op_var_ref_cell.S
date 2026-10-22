// M92: see OP_VAR_REF_CELL's own comment in ir.h and heap.h's
// ASNIX_CELL_MARKER comment for the full rationale. Same De Bruijn
// depth/slot walk as op_var_ref.S's asnix_op_var_ref, but instead of
// reading+forcing the slot's VALUE, this computes the slot's own ADDRESS
// and wraps it in a Cell (asnix_make_cell_value, value_util.c) -- no
// forcing happens here at all, deliberately: aliasing the slot is exactly
// the point, and forcing would defeat it (e.g. `let x = [x]; in x`'s
// list element would blackhole instead of lazily aliasing `x`'s own
// not-yet-finished binding).
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"

    .text

    .globl asnix_op_var_ref_cell
    .type asnix_op_var_ref_cell, @function
asnix_op_var_ref_cell:                  // a = depth, b = slot
    movq %rdi, %rcx                     // rcx = env, walked up `depth` hops
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = depth
.Lvar_ref_cell_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_cell_done
    movq ASNIX_ENV_OFF_UP(%rcx), %rcx
    andq $-8, %rcx                      // mask off ASNIX_ENV_WITH_TAG in case this hop is a with-frame
    decq %rdx
    jmp .Lvar_ref_cell_walk
.Lvar_ref_cell_done:
    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = slot
    leaq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rbx // rbx = &env->values[slot] (callee-saved,
                                         // survives the call below)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes, stays 16-byte aligned)

    movq %rbx, %rdi                     // arg1 = target
    call asnix_make_cell_value

    popq %rdi                           // restore ENV (pushed last, popped first)
    popq %rsi                           // restore IP

    ASNIX_CHECK_VALSTACK %rcx           // %rcx dead (held the slot index, unused since)
    movq %rax, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_var_ref_cell, . - asnix_op_var_ref_cell
