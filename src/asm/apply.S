// M7+: a general re-entrant "call this Value from C" entry point, used by
// builtins (map/filter/foldl' and friends, next stage) that need to invoke
// an arbitrary Nix closure/primop VALUE and get its result back. This is
// the mechanism asnix_force_for_print's own header comment named as
// explicitly out of its scope ("not a general re-entrant builtin-calls-
// force mechanism ... that's M7+") -- force.S forces a thunk with zero
// arguments and memoizes into that thunk cell; this instead applies a
// function value to caller-supplied arguments and has no thunk to
// memoize into, so it gets its own sentinel/epilogue pair rather than
// reusing force.S's force_target convention.
//
// Modeled closely on asnix_force_for_print's self-contained-dispatch-
// context pattern (own fresh VALSTACK+RETSP via asnix_arena_alloc, never
// shared with whatever asnix_eval/asnix_force_for_print/asnix_apply
// invocation might already be active on the native call stack -- this
// may run reentrantly, e.g. a builtin called from within an actively
// running asnix_eval that itself calls back into asnix_apply) and on
// op_call.S's two dispatch paths (Closure: allocate a frame, copy args
// in, push a sentinel RetFrame, jump to the body; PrimOp: call the native
// fn pointer directly, no Env/VALSTACK/RETSP needed at all) -- including
// the same type/arity checks op_call.S makes (own copies of the error
// messages below, since op_call.S's are file-local `.L` labels, but
// worded the same way for a consistent user-facing error style).
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    // Same sizing as force.S's own nested-forcing buffers (see that
    // file's comment and docs/architecture.md's "Known bugs" list for why
    // these are bounds-checked rather than just "large enough"): smaller
    // than dispatch.S's top-level buffers since this context only ever
    // runs one applied-closure's body at a time, but still bounds-checked
    // via the same shared asnix_valstack_limit/asnix_retsp_limit globals
    // the main dispatch loop and force.S use, so a pathological closure
    // body (e.g. one that builds a huge list) fails loudly instead of
    // silently corrupting the arena.
    .equ ASNIX_APPLY_VALSTACK_BYTES, 262144 // 32,768 Values, 256 KiB
    .equ ASNIX_APPLY_RETSTACK_BYTES, 65536  // 2,048 call frames, 64 KiB

    .section .rodata
.Lmsg_apply_type:
    .ascii "asnix: apply target is not a function, aborting\n"
    .equ ASNIX_MSG_APPLY_TYPE_LEN, . - .Lmsg_apply_type
.Lmsg_apply_arity:
    .ascii "asnix: apply argument count does not match arity, aborting\n"
    .equ ASNIX_MSG_APPLY_ARITY_LEN, . - .Lmsg_apply_arity

    .data
    .align 8
.Lapply_sentinel_ir:
    .quad asnix_apply_epilogue
    .quad 0
    .quad 0

    .text

    .globl asnix_apply
    .type asnix_apply, @function
asnix_apply:                            // Value asnix_apply(Value fn, const Value *args, int64_t argc)
                                         // rdi = fn, rsi = args, rdx = argc
    movq %rdi, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_CLOSURE, %rcx
    je .Lapply_is_closure
    cmpq $ASNIX_TAG_PRIMOP, %rcx
    je .Lapply_is_primop
    jmp .Lapply_type_error

    // PrimOp: a plain C function, Value (*)(const Value *args, int64_t
    // argc) -- same convention op_call.S's primop path calls it with (see
    // docs/abi.md's "Primop calls (M7)" section). No Env/VALSTACK/RETSP
    // needed at all (a primop has no lexical body to run through the
    // dispatch loop), so this is a plain tail call: shuffle our own
    // (args, argc) into the native argument registers and jump straight
    // into it, letting its own `ret` return directly to our caller.
.Lapply_is_primop:
    andq $-8, %rax                      // rax = real PrimOp*
    movq ASNIX_PRIMOP_OFF_ARITY(%rax), %rcx
    cmpq %rcx, %rdx
    jne .Lapply_arity_error
    movq ASNIX_PRIMOP_OFF_FN(%rax), %rcx
    movq %rsi, %rdi                      // arg1 = args ptr
    movq %rdx, %rsi                      // arg2 = argc
    jmp *%rcx

    // Closure: needs a real frame and a real dispatch context to run its
    // IR body through, exactly like op_call.S's closure path, except
    // there is no enclosing asnix_eval invocation to borrow VALSTACK/
    // RETSP/the "resume here" IP from -- we bring our own, and use a
    // sentinel RetFrame (mirroring force.S's .Lforce_sentinel_ir/
    // asnix_force_epilogue) so OP_RET's ordinary jmp-chain lands back in
    // native-callable code that returns the result to our C caller.
.Lapply_is_closure:
    andq $-8, %rax                      // rax = real Closure*
    movq ASNIX_CLOSURE_OFF_ARITY(%rax), %rcx
    cmpq %rcx, %rdx
    jne .Lapply_arity_error

    // Save every candidate-scratch register, exactly like asnix_eval's/
    // asnix_force_for_print's own prologues -- this may run when no
    // asnix_eval invocation is live at all, or nested inside one (or
    // inside another asnix_apply / asnix_force_for_print call).
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp                       // alignment padding, undone in the epilogue below

    movq %rax, %rbx                     // rbx = Closure* (survives the calls below)
    movq %rsi, %rbp                     // rbp = args ptr (survives the calls below)
    movq %rdx, %r15                     // r15 = argc (survives the calls below)

    // Save the enclosing dispatch context's VALSTACK/RETSP limits --
    // restored in the epilogue below, mirroring %r12/%r14 themselves (see
    // force.S, which does the same around its own nested use).
    movq asnix_valstack_limit(%rip), %rax
    pushq %rax
    movq asnix_retsp_limit(%rip), %rax
    pushq %rax

    movq $ASNIX_APPLY_VALSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r12
    leaq ASNIX_APPLY_VALSTACK_BYTES(%r12), %rcx
    movq %rcx, asnix_valstack_limit(%rip)

    movq $ASNIX_APPLY_RETSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r14
    leaq ASNIX_APPLY_RETSTACK_BYTES(%r14), %rcx
    movq %rcx, asnix_retsp_limit(%rip)

    // Allocate the new call frame (size = 8 + argc*8, same layout as
    // op_call.S's closure path) and copy args in from our caller's plain
    // C array (%rbp), not from VALSTACK -- there is no caller IrOp that
    // already pushed them there the way an in-language OP_CALL has.
    movq %r15, %rcx
    leaq 8(, %rcx, 8), %rdi
    call asnix_arena_alloc
    movq ASNIX_CLOSURE_OFF_UP(%rbx), %rcx
    movq %rcx, ASNIX_ENV_OFF_UP(%rax)

    xorq %rcx, %rcx                     // rcx = i = 0
.Lapply_copy_args:
    cmpq %r15, %rcx
    jge .Lapply_copy_done
    movq (%rbp, %rcx, 8), %rdx          // arg_i, from the caller's args array
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    incq %rcx
    jmp .Lapply_copy_args
.Lapply_copy_done:

    // Sentinel retframe: an ordinary call (force_target = 0, nothing to
    // memoize) whose IP points at a small epilogue that restores every
    // saved register and returns the result to our C caller.
    ASNIX_CHECK_RETSP %rcx              // %rcx dead here (last written by the copy loop above)
    leaq .Lapply_sentinel_ir(%rip), %rcx
    movq %rcx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq $0, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq $0, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq $0, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq %rax, %rdi                     // ENV = new frame
    movq ASNIX_CLOSURE_OFF_POOL(%rbx), %r13 // POOL = the callee's own pool, not any caller's
    movq ASNIX_CLOSURE_OFF_BODY(%rbx), %rsi // IP = closure body
    movq (%rsi), %rax
    jmp *%rax

asnix_apply_epilogue:
    movq -8(%r12), %rax                 // result, left on VALSTACK by the closure body's own OP_RET
    popq %rcx                           // restore the enclosing context's RETSP limit (pushed
    movq %rcx, asnix_retsp_limit(%rip)  // last above, so popped first)
    popq %rcx                           // ditto for the VALSTACK limit
    movq %rcx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret

.Lapply_type_error:
    leaq .Lmsg_apply_type(%rip), %rdi
    movq $ASNIX_MSG_APPLY_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lapply_arity_error:
    leaq .Lmsg_apply_arity(%rip), %rdi
    movq $ASNIX_MSG_APPLY_ARITY_LEN, %rsi
    jmp asnix_fatal
    .size asnix_apply, . - asnix_apply
