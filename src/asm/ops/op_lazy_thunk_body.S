#include "asnix/ir.h"

    .text

    .globl asnix_op_lazy_thunk_body
    .type asnix_op_lazy_thunk_body, @function
asnix_op_lazy_thunk_body:               // b = pool index of a LazyThunkDescriptor*
                                         // (docs/lazy-thunk-compile-proposal.md). The ONLY op
                                         // in a stub IrProgram -- entered with %rsi/%r13 =
                                         // THIS stub's own 1-op ops/1-entry pool, %rdi = the
                                         // calling thunk's real captured ENV (already set up
                                         // by whichever force-entry site jumped here, same as
                                         // for any other thunk body), %r12/%r14 = the live
                                         // VALSTACK/RETSP (a RETFRAME was already pushed by
                                         // that same force-entry site before the jump -- this
                                         // handler itself pushes nothing onto either stack).
    movl  ASNIX_IROP_OFF_B(%rsi), %ecx
    movq  (%r13, %rcx, 8), %rdx         // rdx = LazyThunkDescriptor* (NOT %rdi -- %rdi still
                                         // holds the real ENV, which must survive this call so
                                         // the real program's first op sees the correct
                                         // captured scope, the same way it would have if this
                                         // thunk had been compiled eagerly all along; this is a
                                         // real fix vs the design doc's own draft, which
                                         // clobbered %rdi with the descriptor pointer before
                                         // saving it, losing ENV across the call -- found by
                                         // tracing registers against the SysV/this-project's
                                         // own calling convention, not assumed safe)
    pushq %rsi                          // save IP (dead afterward, but mirrors the project's
                                         // existing 2-push/16-byte-aligned-call convention,
                                         // e.g. op_mk_thunk.S)
    pushq %rdi                          // save the REAL ENV
    movq  %rdx, %rdi                    // %rdi = LazyThunkDescriptor* -- the call's one arg
    call  asnix_compile_lazy_thunk_body // C bridge (compiler.c): compiles the deferred subtree
                                         // for real, mutates desc->self's ops/pool in place,
                                         // returns the SAME IrProgram* (desc->self)
    popq  %rdi                          // restore the REAL ENV
    popq  %rsi                          // restore IP (unused below)
    movq  %rax, %rcx                    // rcx = desc->self, now carrying REAL ops/pool
    movq  ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq  ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq  (%rsi), %rax
    jmp   *%rax                         // fall into the real first op, in the SAME dispatch
                                         // context -- no asnix_apply, no new RETFRAME (this is
                                         // the exact property M187's own "Conclusion" asked for)
    .size asnix_op_lazy_thunk_body, . - asnix_op_lazy_thunk_body
