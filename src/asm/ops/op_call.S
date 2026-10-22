#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_call_arity:
    .ascii "asnix: call argument count does not match arity, aborting\n"
    .equ ASNIX_MSG_CALL_ARITY_LEN, . - .Lmsg_call_arity

    .text

    .globl asnix_op_call
    .type asnix_op_call, @function
asnix_op_call:                          // a = argc
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = argc (callee-saved, survives the call below)

    // Locate the callee: VALSTACK[top - argc - 1] (args were pushed after it).
    movq %rbx, %rax
    incq %rax
    shlq $3, %rax
    movq %r12, %rcx
    subq %rax, %rcx                     // rcx = address of the callee's slot
    movq (%rcx), %rdx                   // rdx = callee Value (tagged)

    movq %rdx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_CLOSURE, %r8
    je .Lop_call_is_closure
    cmpq $ASNIX_TAG_PRIMOP, %r8
    je .Lop_call_is_primop
    cmpq $ASNIX_TAG_ATTRS, %r8
    je .Lop_call_is_attrs
    jmp .Lop_call_type_error

    // `__functor` protocol (M42): `f arg` where `f` is an attrset with a
    // `__functor` attribute -- see asnix_bshared_call_functor's own
    // comment (builtins_shared.c) for the full mechanism. Only argc==1
    // is supported (every ordinary Nix-level call site is arity-1
    // anyway, per parser.c's parse_app always desugaring multi-arg
    // application into nested arity-1 calls); falls through to the
    // ordinary type error otherwise. A real `call` (not a tail `jmp`),
    // since -- unlike apply.S's own version of this check, which can
    // tail-call straight back to ITS caller -- this handler needs
    // control back afterward to push the result onto VALSTACK and
    // continue the dispatch loop.
.Lop_call_is_attrs:
    cmpq $1, %rbx
    jne .Lop_call_type_error
    movq %rcx, %rbx                      // rbx = callee slot addr (callee-saved, survives the call;
                                          // argc check is already done, so rbx is free to reuse)
    movq 8(%rbx), %r9                    // r9 = the single argument's Value (caller-saved, but only
                                          // needs to survive up to the `movq %r9, %rsi` just below)

    pushq %rsi                           // save IP
    pushq %rdi                           // save ENV (2 pushes, stays 16-byte aligned)
    movq %rdx, %rdi                      // arg1 = attrs_val (still tagged, loaded above)
    movq %r9, %rsi                       // arg2 = the argument
    call asnix_bshared_call_functor
    popq %rdi                            // restore ENV
    popq %rsi                            // restore IP

    movq %rbx, %r12                      // pop callee+arg off VALSTACK in one shot
    ASNIX_CHECK_VALSTACK %r8             // %r8 dead (held the tag-mask copy, unused since)
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_is_primop:
    andq $-8, %rdx                      // rdx = real PrimOp*
    movq ASNIX_PRIMOP_OFF_ARITY(%rdx), %r8
    cmpq %r8, %rbx
    jne .Lop_call_arity_error

    movq ASNIX_PRIMOP_OFF_FN(%rdx), %rbp // rbp = native fn ptr (callee-saved, survives the call below)
    movq %rcx, %r15                      // r15 = callee slot address (args start at r15+8)

    pushq %rsi                           // save IP
    pushq %rdi                           // save ENV (2 pushes, stays 16-byte aligned)

    // M25: force each argument before handing it to the native C
    // function. Native primops read args[i]'s raw tag directly -- they
    // have no way to force a thunk themselves (that needs the full
    // dispatch machinery, see asnix_force_for_print, src/asm/force.S).
    // Previously a no-op in practice: every argument reaching here was
    // already forced by construction, since call arguments used to be
    // compiled eagerly (call-by-value). Now that compiler.c's AST_APP
    // wraps every argument in a genuine thunk (M25, for real Nix's
    // call-by-need semantics -- e.g. lazy fixed points like `let f = x:
    // {a=1;}; self = f self; in self.a`), a primop could otherwise
    // receive an unforced Thunk and misread its raw bits as if it were
    // already the right tag -- silent corruption, not a clean error.
    // %r9 is a loop counter spilled to a fixed one-time-allocated stack
    // slot (not push/pop'd per iteration) specifically to keep %rsp
    // 16-byte aligned across every `call` here, matching this file's
    // existing push-in-pairs discipline.
    //
    // M30: skipped entirely when ASNIX_PRIMOP_OFF_NOFORCE is set --
    // `tryEval` is the one primop that needs its argument raw/unforced,
    // so it can force it itself inside its own setjmp/longjmp catch
    // scope (builtins_tryeval.c). Every other primop leaves this flag 0
    // and is unaffected.
    cmpq $0, ASNIX_PRIMOP_OFF_NOFORCE(%rdx)
    jne .Lop_call_force_args_done
    subq $16, %rsp
    xorq %r9, %r9
.Lop_call_force_args:
    cmpq %rbx, %r9
    jge .Lop_call_force_args_done_pop
    movq 8(%r15, %r9, 8), %rdi
    movq %r9, (%rsp)
    call asnix_force_for_print
    movq (%rsp), %r9
    movq %rax, 8(%r15, %r9, 8)
    incq %r9
    jmp .Lop_call_force_args
.Lop_call_force_args_done_pop:
    addq $16, %rsp
.Lop_call_force_args_done:

    leaq 8(%r15), %rdi                   // arg1 = args ptr (straight into VALSTACK, no Env copy)
    movq %rbx, %rsi                      // arg2 = argc
    call *%rbp
    // M23: force the primop's raw return value before it ever reaches
    // VALSTACK. Most primops already return a forced value (nothing to
    // do here -- asnix_force_for_print's own fast path is a single tag
    // check), but a "lazy passthrough" primop like getAttr deliberately
    // returns an attrset entry's raw (possibly still-unforced) Thunk
    // unchanged, matching real Nix's own lazy `.`/getAttr semantics.
    // Every OTHER value producer in this evaluator (OP_VAR_REF,
    // OP_SELECT, a closure's own OP_RET) already forces inline before
    // leaving a result on VALSTACK, so operators like `+` (op_arith.S)
    // only ever check the raw tag directly, never re-forcing -- that
    // invariant silently didn't hold for a primop's result, so a getAttr
    // result flowing straight into `+`/any other operator surfaced as
    // "cannot coerce this value to a string" instead of the real, forced
    // value (found via numtide/nix-conformance's eval-okay-attrnames.nix,
    // completely unrelated to any one specific primop -- any primop that
    // returns a reference without forcing it hits this). asnix_force_for_
    // print (src/asm/force.S) itself preserves every register this
    // routine still needs afterward (%rbx/%rbp/%r15/%r12/%r13/%r14), so
    // no extra save/restore is needed around this call.
    movq %rax, %rdi
    call asnix_force_for_print
    popq %rdi
    popq %rsi

    movq %r15, %r12                      // pop callee+args off VALSTACK in one shot
    ASNIX_CHECK_VALSTACK %rcx            // %rcx dead (copied into %r15 earlier, never reread)
    movq %rax, (%r12)                    // push the primop's result
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_is_closure:
    andq $-8, %rdx                      // rdx = real Closure* (arena pointers are 8-byte aligned)

    movq ASNIX_CLOSURE_OFF_ARITY(%rdx), %r8
    cmpq %r8, %rbx
    jne .Lop_call_arity_error

    movq %rdx, %rbp                     // rbp = closure ptr (callee-saved, survives the call below)
    movq %rcx, %r15                     // r15 = callee slot address (callee-saved, survives the call below)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV == the return_env (2 pushes, stays 16-byte aligned)

    movq %rbx, %rcx
    leaq 8(, %rcx, 8), %rdi             // size = 8 + argc*8
    call asnix_arena_alloc
    movq ASNIX_CLOSURE_OFF_UP(%rbp), %rcx
    movq %rcx, ASNIX_ENV_OFF_UP(%rax)

    xorq %rcx, %rcx                     // rcx = i = 0
.Lop_call_copy_args:
    cmpq %rbx, %rcx
    jge .Lop_call_copy_done
    movq 8(%r15, %rcx, 8), %rdx         // arg_i, still sitting where it was pushed
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    incq %rcx
    jmp .Lop_call_copy_args
.Lop_call_copy_done:

    movq %r15, %r12                     // pop callee+args off VALSTACK in one shot

    popq %rdx                           // saved ENV (return_env)
    popq %rcx                           // saved IP
    addq $ASNIX_IROP_SIZE, %rcx         // return_ip = saved IP past this CALL record

    ASNIX_CHECK_RETSP %r8               // %r8 dead (held arity for the comparison above, unused since)
    movq %rcx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdx, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14) // save the caller's constant pool too (see ir.h)
    movq $0, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14) // ordinary call: no thunk to memoize on return
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq %rax, %rdi                     // ENV = new frame
    // M100: prog (one pointer) replaces {body,pool} -- %rcx is free here
    // (held return_ip, already written into the retframe above).
    movq ASNIX_CLOSURE_OFF_PROG(%rbp), %rcx
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13 // POOL = the callee's own pool, not the caller's
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi  // IP = closure body
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_type_error:
    // M50: names the actual type of the uncallable target (real `call`
    // into C, same reasoning as op_select.S's .Lselect_type_error/M36:
    // %rsp is already 16-byte aligned entering asnix_op_call -- nothing
    // has been pushed yet on this path -- and asnix_bshared_call_type_
    // fatal is _Noreturn, so the pushed return address is never used).
    // %rdx still holds the original tagged callee Value here (loaded
    // above, before any masking).
    movq %rdx, %rdi
    call asnix_bshared_call_type_fatal
.Lop_call_arity_error:
    leaq .Lmsg_call_arity(%rip), %rdi
    movq $ASNIX_MSG_CALL_ARITY_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_call, . - asnix_op_call

    .globl asnix_op_ret
    .type asnix_op_ret, @function
asnix_op_ret:
    subq $ASNIX_RETFRAME_SIZE, %r14
    movq ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14), %rcx
    testq %rcx, %rcx
    jz .Lret_no_memo
    // This frame was pushed to force a thunk (OP_VAR_REF/OP_SELECT), not
    // an ordinary call -- the body's last expression already left its
    // result on top of VALSTACK (same place an ordinary call leaves its
    // result); memoize it into the thunk cell before resuming.
    //
    // M82: this thunk (%rcx) is always exactly the current head of
    // `asnix_inprogress_head`'s intrusive stack (stacklimits.S) here --
    // forcing is strictly LIFO under normal (non-exceptional)
    // completion, so whichever thunk most recently went IN_PROGRESS is
    // always the next one to finish -- so pop it (read its "prev" link,
    // stashed in ENV while IN_PROGRESS, BEFORE overwriting ENV with the
    // cached result below) rather than leaving it stuck on the list
    // forever, which would corrupt the chain for the NEXT thunk's own
    // push/pop (found exactly this way: a successfully-forced thunk
    // left dangling on the list, its `env` field later holding a cached
    // Value instead of a valid "prev" pointer, crashing
    // asnix_tryeval_unwind_inprogress's walk on an unrelated later
    // `tryEval` catch). %rax is dead here (last loaded by OP_CALL's own
    // dispatch, several ops ago).
    movq ASNIX_THUNK_OFF_ENV(%rcx), %rax
    movq %rax, asnix_inprogress_head(%rip)
    movq -8(%r12), %rdx
    movq %rdx, ASNIX_THUNK_OFF_ENV(%rcx)
    movq $ASNIX_THUNK_STATE_FORCED, ASNIX_THUNK_OFF_STATE(%rcx)
.Lret_no_memo:
    movq ASNIX_RETFRAME_OFF_IP(%r14), %rsi
    movq ASNIX_RETFRAME_OFF_ENV(%r14), %rdi
    movq ASNIX_RETFRAME_OFF_POOL(%r14), %r13
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ret, . - asnix_op_ret
