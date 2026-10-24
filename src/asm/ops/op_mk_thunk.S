#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_thunk
    .type asnix_op_mk_thunk, @function
asnix_op_mk_thunk:                      // b = pool index of {body IrProgram*} (M100: a single
                                         // pointer, not a {body IrOp*, body pool} pair)
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq (%r13, %rcx, 8), %rbp          // rbp = body IrProgram* (callee-saved, survives the call below)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV -- this thunk's captured scope (2 pushes, aligned)

    movq $ASNIX_THUNK_SIZE, %rdi
    call asnix_arena_alloc
    incq asnix_stats_thunk(%rip)
    // M103: OFF_STATE now doubles as the prog pointer while UNFORCED --
    // %rbp (an IrProgram*) is always 8-byte-aligned, so its low 3 bits
    // are already 0 == ASNIX_THUNK_STATE_UNFORCED; one store, not two
    // (see ir.h's comment).
    movq %rbp, ASNIX_THUNK_OFF_STATE(%rax)

    popq %rdi                           // restore ENV (pushed last, popped first) -- see the
                                         // comment in op_lambda.S's matching fix for why this
                                         // must land in %rdi itself, not just get copied into
                                         // the heap object
    movq %rdi, ASNIX_THUNK_OFF_ENV(%rax)
    popq %rsi                           // restore IP

    movq %rax, %rdx
    orq $ASNIX_TAG_THUNK, %rdx
    ASNIX_CHECK_VALSTACK %rcx           // %rcx dead (held the pool index, unused since)
    movq %rdx, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_thunk, . - asnix_op_mk_thunk
