#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed

    .text

    .globl asnix_op_var_ref
    .type asnix_op_var_ref, @function
asnix_op_var_ref:                       // a = depth, b = slot
    movq %rdi, %rcx                     // rcx = env, walked up `depth` hops
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = depth
.Lvar_ref_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_done
    movl ASNIX_ENV_OFF_UP(%rcx), %ecx    // M118: zero-extends into %rcx; decompress below
    addq asnix_env_arena_base(%rip), %rcx
    andq $-2, %rcx                      // mask off ASNIX_ENV_WITH_TAG in case this hop is a with-frame
    decq %rdx
    jmp .Lvar_ref_walk
.Lvar_ref_done:
    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = slot
    movq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rax // rax = raw tagged value

.Lvar_ref_resolve:                      // M92: re-entered after unwrapping a Cell (see
                                         // .Lvar_ref_check_cell below) -- the Cell's target may
                                         // itself still need forcing, or even be another Cell
                                         // (chained bare-var aliasing, e.g. `let y=x; z=y;`).
    // M6: force if this slot holds an unforced thunk (laziness).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lvar_ref_check_cell

    andq $-8, %rax                      // rax = Thunk*
    // M103: OFF_STATE is now tagged -- UNFORCED: this word IS the clean
    // IrProgram* (tag bits 0); IN_PROGRESS/FORCED/FAILED: the bare state
    // tag alone. Extract the tag into %r8 for dispatch while keeping the
    // (possibly-a-pointer) original word in %rcx -- on the UNFORCED path
    // below, %rcx already equals the clean prog pointer with no masking.
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lvar_ref_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lvar_ref_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lvar_ref_reraise

    // UNFORCED: enter the thunk's body like a zero-arg call, with a
    // retframe that tells OP_RET to memoize the result back into this
    // cell instead of just resuming (see op_call.S). %rcx (the clean
    // prog pointer) must survive this -- ASNIX_CHECK_RETSP below is
    // given %r8 (dead: already consumed above) as scratch instead of
    // %rcx.
    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx         // return_ip = one past this VAR_REF record
    ASNIX_CHECK_RETSP %r8               // %r8 dead (already consumed into the state tag above)
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)     // ENV/POOL are unchanged by var_ref itself
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    // M103: %rcx already holds the clean prog pointer (read above, tag
    // bits were 0) -- no second memory read needed (the old separate
    // OFF_PROG field is gone).
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi       // IP = thunk body
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi         // ENV = thunk's captured scope -- last read of this
                                                  // field's ORIGINAL meaning; safe to repurpose below.
    // M82: push this thunk onto the intrusive IN_PROGRESS stack (see
    // stacklimits.S's asnix_inprogress_head comment) -- %rcx is dead
    // here (overwritten immediately below, same as before this M100 change).
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rax)
    movq %rax, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax // cached Value.raw (ENV slot repurposed once forced)
    // perf: skip straight to the push path instead of re-entering
    // .Lvar_ref_resolve's thunk-tag/Cell-tag checks. Safe by construction,
    // not just "probably fine": a raw Cell (heap.h's ASNIX_CELL_MARKER) is
    // only ever produced by OP_VAR_REF_CELL at a would-be thunk-creation
    // site (compiler.c's try_cell_ref) -- it SUBSTITUTES for creating a
    // Thunk there, so no Thunk object exists to force in that case at all.
    // A real Thunk's body is always compiled via ordinary compile_expr
    // (never try_cell_ref's short-circuit), so by the time OP_RET memoizes
    // its result into ASNIX_THUNK_OFF_ENV, that value is guaranteed to be
    // neither a Thunk (already fully resolved by the body's own
    // VAR_REF/SELECT) nor a Cell (no code path ever produces one there).
    // This was previously a defensive re-check paid on every single access
    // to an already-forced thunk -- one of the hottest paths in the whole
    // evaluator -- for an invariant that already held unconditionally.
    jmp .Lvar_ref_push

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment). %rcx still holds the tag bits from the
    // check just above (the only path that reaches this label).
.Lvar_ref_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lvar_ref_push
    movq %rax, %r8
    andq $-8, %r8                        // clear the low 3 tag bits...
    shlq $16, %r8                        // ...and the high 16 bits a packed M118 ThunkMap word's
    shrq $16, %r8                        // index occupies (a no-op for every other PTR kind --
                                          // see heap.h's ASNIX_THUNKMAP_MARKER comment) --
                                          // r8 = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lvar_ref_check_lazyapp          // PTR but not a Cell -- maybe a LazyApp instead

    // It's a Cell -- M162: inline Cell-chain walk, hand-rolled directly
    // from asnix_resolve_cell_chain/asnix_value_is_cell/asnix_cell_target
    // (value_util.c) instead of `call`ing out to them. Those three tiny
    // functions were never inlined into each other by GCC (confirmed via
    // objdump -- asnix_resolve_cell_chain's loop body was `call
    // asnix_cell_target` then, on the next iteration test, `call
    // asnix_value_is_cell`, which ITSELF did `call asnix_tag_of`), so
    // every chase of a bare-variable alias paid 1-3 nested out-of-line
    // calls for what's really just "mask the tag bits, load, compare,
    // branch" -- 834,237 Cells created on this project's own d.nix
    // workload, each read at least once. %r8 already holds the masked
    // base pointer of a CONFIRMED Cell (the check just above), so the
    // first dereference below needs no re-verification; the loop only
    // re-checks "is the new value ALSO a Cell" for the (rare) chained-
    // alias case (`let y = x; z = y;`). Unwraps to the first non-Cell
    // Value, which may still be an unforced Thunk/LazyApp/ThunkMap; falls
    // back into .Lvar_ref_resolve to handle that, exactly like the old
    // call-based version did.
    //
    // %rdx is free here (last written by .Lvar_ref_done's slot
    // computation, dead ever since) -- used as the chase-limit counter,
    // matching ASNIX_CELL_CHASE_LIMIT (heap.h) so a genuine alias cycle
    // (`let x = y; y = x; in x` -- a real user-writable program, not a
    // hypothetical) still aborts loudly instead of hanging forever.
    movq $ASNIX_CELL_CHASE_LIMIT, %rdx
.Lvar_ref_cell_loop:
    movq ASNIX_CELL_OFF_TARGET(%r8), %r10 // r10 = target pointer (Value* into an Env's values[])
    movq (%r10), %rax                     // rax = *target -- the actual next raw Value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lvar_ref_resolve                 // not even a PTR -- can't be a Cell, done chasing
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8                         // r8 = masked base pointer of the new value
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lvar_ref_resolve                 // PTR but not a Cell -- done, re-dispatch on %rax
    decq %rdx
    jnz .Lvar_ref_cell_loop
    jmp .Lvar_ref_blackhole               // chase limit exceeded -- a genuine alias cycle

    // M113: not a Cell either -- maybe a LazyApp (heap.h's
    // ASNIX_LAZYAPP_MARKER comment). %r9 already holds the marker value
    // from the Cell check just above. Both LazyApp sub-states
    // (unresolved/resolved) route through the same asnix_resolve_lazyapp
    // call, which may itself hand back another not-yet-forced value
    // (Thunk/Cell/LazyApp) -- re-enter the resolve loop above, same as
    // the Cell case.
.Lvar_ref_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %r9
    je .Lvar_ref_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %r9
    je .Lvar_ref_lazyapp
    jmp .Lvar_ref_check_thunkmap         // PTR but not Cell/LazyApp -- maybe a ThunkMap instead
.Lvar_ref_lazyapp:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi                      // arg1 = the LazyApp value
    call asnix_resolve_lazyapp
    popq %rdi
    popq %rsi
    jmp .Lvar_ref_resolve

    // M118: not a Cell/LazyApp either -- maybe a ThunkMap (heap.h's
    // ASNIX_THUNKMAP_MARKER comment). %r9 already holds the marker value
    // from the Cell check above (%r8, computed there, is ALSO still the
    // fully-masked base pointer -- see op_var_ref.S's own matching
    // comment on that mask).
.Lvar_ref_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %r9
    jne .Lvar_ref_push                   // PTR but not Cell/LazyApp/ThunkMap: a real value
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi                      // arg1 = the ThunkMap value (original, unmasked)
    call asnix_resolve_thunkmap
    popq %rdi
    popq %rsi
    jmp .Lvar_ref_resolve
.Lvar_ref_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lvar_ref_reraise:
    // M82: re-forcing a thunk whose previous force failed (`throw`/a
    // failed `assert`, caught by an enclosing `tryEval` since Nix 2.34
    // memoizes failures too) -- not a genuine cycle, and must not re-run
    // the body (no duplicate side effects): re-raise, catchable again by
    // whatever `tryEval` is active now (or fatal if none is).
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_var_ref, . - asnix_op_var_ref
