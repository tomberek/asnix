// `rec { ... __overrides = {...}; ... }` (M57c) -- see OP_REC_OVERRIDES_MERGE's
// long comment (ir.h) for the full picture. Deferred to C
// (asnix_bshared_rec_overrides_merge, builtins_shared.c) for the same
// reason OP_UPDATE/OP_INSERT_DYNAMIC_ATTR are: not a hot-path op, and the
// attrset-merge logic is far easier to get right as plain C.
//
// Unlike every other "call into C" op in this directory, this one pops
// NOTHING off VALSTACK -- its inputs are the CURRENT Env's own slots
// (already filled in by the OP_FRAME_SET loop compiler.c emitted just
// before this op), not values pushed by the compiler. IP (%rsi) and ENV
// (%rdi) are still NOT callee-saved across the `call`, so they're still
// saved/restored the same push/pop way op_insert_dynamic_attr.S does --
// just computed into temp registers (%rax = n, %rcx = meta pointer)
// *before* %rsi's IP is overwritten, since both operands live in the
// IrOp %rsi currently points at.
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_rec_overrides_merge
    .type asnix_op_rec_overrides_merge, @function
asnix_op_rec_overrides_merge:       // a = n, b = pool index of [n syms][overrides_slot]
    movq ASNIX_IROP_OFF_A(%rsi), %rax   // rax = n (temp; becomes arg2)
    movq ASNIX_IROP_OFF_B(%rsi), %rcx   // rcx = pool_idx
    leaq (%r13, %rcx, 8), %rcx           // rcx = &pool[pool_idx] = meta pointer (temp; becomes arg3)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
                                         // %rdi is already ENV == arg1, untouched since entry
    movq %rax, %rsi                     // arg2 = n
    movq %rcx, %rdx                     // arg3 = meta
    call asnix_bshared_rec_overrides_merge
    popq %rdi                           // ENV
    popq %rsi                           // IP

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_rec_overrides_merge, . - asnix_op_rec_overrides_merge
