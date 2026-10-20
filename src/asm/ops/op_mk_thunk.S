#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_thunk
    .type asnix_op_mk_thunk, @function
asnix_op_mk_thunk:                      // b = pool index of {body IrOp*, body pool}
    movq ASNIX_IROP_OFF_B(%rsi), %rcx
    movq (%r13, %rcx, 8), %rbp          // rbp = body pointer (callee-saved, survives the call below)
    movq 8(%r13, %rcx, 8), %r15         // r15 = body's own constant pool (ditto)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV -- this thunk's captured scope (2 pushes, aligned)

    movq $ASNIX_THUNK_SIZE, %rdi
    call asnix_arena_alloc
    movq $ASNIX_THUNK_STATE_UNFORCED, ASNIX_THUNK_OFF_STATE(%rax)
    movq %rbp, ASNIX_THUNK_OFF_BODY(%rax)
    movq %r15, ASNIX_THUNK_OFF_POOL(%rax)

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
