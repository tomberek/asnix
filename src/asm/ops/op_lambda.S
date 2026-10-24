#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_lambda
    .type asnix_op_lambda, @function
asnix_op_lambda:                        // a = arity, b = pool index of {body IrProgram*,
                                         // formals} (M100: prog is a single pointer, not a
                                         // {body IrOp*, body pool} pair)
    movl ASNIX_IROP_OFF_A(%rsi), %ebx   // rbx = arity (callee-saved, survives the call below)
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq (%r13, %rcx, 8), %rbp          // rbp = body IrProgram* (callee-saved, survives the call below)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stack stays 16-byte aligned)

    movq $ASNIX_CLOSURE_SIZE, %rdi
    call asnix_arena_alloc
    incq asnix_stats_closure(%rip)
    movq %rbp, ASNIX_CLOSURE_OFF_PROG(%rax)
    movq %rbx, ASNIX_CLOSURE_OFF_ARITY(%rax)

    popq %rdi                           // restore ENV (pushed last, popped first) -- %rdi is the
                                         // live current-ENV register for whatever runs next, not
                                         // just scratch; previously popped into %rdx and only
                                         // written into the closure, leaving %rdi holding the
                                         // $ASNIX_CLOSURE_SIZE size-argument garbage (a real bug,
                                         // latent until M6 exercised a path that read it)
    movq %rdi, ASNIX_CLOSURE_OFF_UP(%rax)
    popq %rsi                           // restore IP

    // M37: the pre-built `builtins.functionArgs` Value lives in this
    // closure's OWN pool (pool_idx+1, M100: shifted down from pool_idx+2
    // now that prog collapses what were two pool entries into one --
    // see ib_lambda/irbuild.c) -- reload it fresh here rather than
    // trying to carry it in a register across the `call` above: %rcx
    // (pool index) and %rdx are caller-saved (not guaranteed to survive
    // asnix_arena_alloc), while every register that genuinely needs to
    // survive the call (arity/prog/IP/ENV) already has a callee-saved
    // home or an explicit push/pop above -- recomputing from %rsi (just
    // restored) and %r13 (POOL, callee-saved, untouched by the call) is
    // simpler and avoids needing a 4th callee-saved scratch register
    // (%rbx/%rbp are already spoken for) or extra stack padding.
    movl ASNIX_IROP_OFF_B(%rsi), %ecx
    movq 8(%r13, %rcx, 8), %rdx
    movq %rdx, ASNIX_CLOSURE_OFF_FORMALS(%rax)

    movq %rax, %rdx
    orq $ASNIX_TAG_CLOSURE, %rdx
    ASNIX_CHECK_VALSTACK %rcx           // %rcx dead (held the pool index, unused since)
    movq %rdx, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_lambda, . - asnix_op_lambda
