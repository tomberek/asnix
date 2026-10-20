#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_lambda
    .type asnix_op_lambda, @function
asnix_op_lambda:                        // a = arity, b = pool index of {body IrOp*, body pool}
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = arity (callee-saved, survives the call below)
    movq ASNIX_IROP_OFF_B(%rsi), %rcx
    movq (%r13, %rcx, 8), %rbp          // rbp = body pointer (callee-saved, survives the call below)
    movq 8(%r13, %rcx, 8), %r15         // r15 = body's own constant pool (ditto; see ir.h)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stack stays 16-byte aligned)

    movq $ASNIX_CLOSURE_SIZE, %rdi
    call asnix_arena_alloc
    movq %rbp, ASNIX_CLOSURE_OFF_BODY(%rax)
    movq %rbx, ASNIX_CLOSURE_OFF_ARITY(%rax)
    movq %r15, ASNIX_CLOSURE_OFF_POOL(%rax)

    popq %rdi                           // restore ENV (pushed last, popped first) -- %rdi is the
                                         // live current-ENV register for whatever runs next, not
                                         // just scratch; previously popped into %rdx and only
                                         // written into the closure, leaving %rdi holding the
                                         // $ASNIX_CLOSURE_SIZE size-argument garbage (a real bug,
                                         // latent until M6 exercised a path that read it)
    movq %rdi, ASNIX_CLOSURE_OFF_UP(%rax)
    popq %rsi                           // restore IP

    movq %rax, %rdx
    orq $ASNIX_TAG_CLOSURE, %rdx
    ASNIX_CHECK_VALSTACK %rcx           // %rcx dead (held the pool index, unused since)
    movq %rdx, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_lambda, . - asnix_op_lambda
