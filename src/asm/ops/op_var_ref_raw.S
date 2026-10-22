// M92: see OP_VAR_REF_RAW's own comment in ir.h. Same De Bruijn
// depth/slot walk as op_var_ref.S's asnix_op_var_ref, but pushes the
// slot's raw content verbatim -- no thunk-forcing, no Cell-resolving,
// nothing. The OP_FRAME_GET of depth-relative reads.
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"

    .text

    .globl asnix_op_var_ref_raw
    .type asnix_op_var_ref_raw, @function
asnix_op_var_ref_raw:                   // a = depth, b = slot
    movq %rdi, %rcx                     // rcx = env, walked up `depth` hops
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = depth
.Lvar_ref_raw_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_raw_done
    movl ASNIX_ENV_OFF_UP(%rcx), %ecx    // M118: zero-extends into %rcx; decompress below
    addq asnix_env_arena_base(%rip), %rcx
    andq $-2, %rcx                      // mask off ASNIX_ENV_WITH_TAG in case this hop is a with-frame
    decq %rdx
    jmp .Lvar_ref_raw_walk
.Lvar_ref_raw_done:
    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = slot
    movq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rax // rax = raw tagged value, unforced

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_var_ref_raw, . - asnix_op_var_ref_raw
