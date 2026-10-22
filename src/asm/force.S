// M6: a C-callable force entry point, used by the top-level printer
// (main.c) to recursively force thunks nested inside lists/attrsets for
// display -- the only thing that needs to force a value from *outside* an
// active asnix_eval dispatch loop so far (ordinary in-language forcing
// happens inline in op_var_ref.S/op_select.S). This is intentionally
// narrow: it's not a general re-entrant builtin-calls-force mechanism
// (that's M7+, once real builtins exist and need it); it just sets up its
// own self-contained mini dispatch context (mirroring asnix_eval's own
// prologue) to run one thunk's body to completion and return the result.
#include "asnix/force_scratch.h" // ASNIX_FORCE_{VALSTACK,RETSTACK}_BYTES, per-depth cache (perf)
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    // Same silent-corruption-to-loud-abort fix as dispatch.S's main
    // VALSTACK/RETSP (see include/asnix/stackcheck.h and
    // docs/architecture.md's "Known bugs" list): previously 4096/1024
    // bytes with no bounds check; now much larger (still smaller than
    // dispatch.S's, since this context only ever runs one thunk-forcing
    // sub-dispatch at a time) and bounds-checked via the same shared
    // asnix_valstack_limit/asnix_retsp_limit globals the main dispatch
    // loop uses -- save/restore them around this nested use exactly like
    // %r12/%r14 themselves, since this routine can run nested inside an
    // active asnix_eval (a builtin forcing a value, e.g. derivation.c) or
    // even inside another asnix_force_for_print call.
    //
    // asnix_force_depth: how many asnix_force_for_print frames
    // (including this one, once past the increment below) are currently
    // live, nested inside each other -- doubles as the index into
    // force_scratch.c's per-depth buffer cache (see
    // include/asnix/force_scratch.h's header comment for why indexing by
    // depth, not a single cached slot, turned out to be necessary: `perf
    // record -e page-faults -g` on d.nix showed most real calls here ARE
    // nested a few levels deep via derivation.c, not top-level/flat).
    .bss
    .align 8
    .globl asnix_force_depth
asnix_force_depth:
    .quad 0

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed

    .data
    .align 8
.Lforce_sentinel_ir:
    .quad asnix_force_epilogue
    .quad 0
    .quad 0

    .text

    .globl asnix_force_for_print
    .type asnix_force_for_print, @function
asnix_force_for_print:                  // Value asnix_force_for_print(Value v)
.Lforce_top:                            // M23: re-entered (tail-loop, see .Lforce_done below)
                                         // whenever a force's *result* turns out to itself be
                                         // another unforced Thunk -- found via a lazy builtin
                                         // (map/mapAttrs, M23) whose thunk body calls a
                                         // lazy-pass-through primop (builtins.getAttr, which
                                         // deliberately returns an attrset entry's raw value
                                         // without forcing it -- correct and lazy on its own),
                                         // so forcing ONE level could leave a Thunk cached as
                                         // another Thunk's "result" where previously (every
                                         // ordinary compiled thunk body always forces via
                                         // OP_VAR_REF/OP_SELECT before its own OP_RET) that
                                         // could never happen. Real Nix's own `forceValue` is
                                         // itself a `while (v.isThunk())` loop for exactly this
                                         // reason -- this was a real, latent gap in the
                                         // single-force assumption, just never previously
                                         // reachable.
    movq %rdi, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lforce_done                    // not a thunk: return v unchanged

    andq $-8, %rax                      // rax = Thunk*
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lforce_cached
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lforce_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %rcx
    je .Lforce_reraise

    // Bring our own VALSTACK/RETSP and save every candidate-scratch
    // register, exactly like asnix_eval's prologue (see docs/abi.md) --
    // this call may run when no asnix_eval invocation is live at all.
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp                       // alignment padding, undone in the epilogue below
    movq %rax, %rbx                     // stash the Thunk* across the alloc calls

    // Save the enclosing dispatch context's VALSTACK/RETSP limits (the
    // outer asnix_eval, or an outer asnix_force_for_print if this call
    // nests inside another one) -- restored in the epilogue below,
    // mirroring how %r12/%r14 themselves are saved/restored. %rax holds
    // the live Thunk* here (see above); %rdx is free.
    movq asnix_valstack_limit(%rip), %rdx
    pushq %rdx
    movq asnix_retsp_limit(%rip), %rdx
    pushq %rdx

    // perf: asnix_force_scratch_{valstack,retstack} (force_scratch.c)
    // cache one buffer pair PER DEPTH instead of this routine minting a
    // brand-new pair from the never-free bump arena on every single
    // call -- see force_scratch.h's header comment. %rbx (the live
    // Thunk*) survives both calls: ordinary SysV calls, and %rbx is
    // callee-saved.
    movq asnix_force_depth(%rip), %rdi
    call asnix_force_scratch_valstack
    movq %rax, %r12
    leaq ASNIX_FORCE_VALSTACK_BYTES(%r12), %rdx
    movq %rdx, asnix_valstack_limit(%rip)

    movq asnix_force_depth(%rip), %rdi
    call asnix_force_scratch_retstack
    movq %rax, %r14
    leaq ASNIX_FORCE_RETSTACK_BYTES(%r14), %rdx
    movq %rdx, asnix_retsp_limit(%rip)

    incq asnix_force_depth(%rip)
    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rbx)

    // Sentinel retframe: when the thunk's OP_RET fires, it memoizes into
    // force_target (this thunk) then jumps to the sentinel's handler.
    leaq .Lforce_sentinel_ir(%rip), %rax
    ASNIX_CHECK_RETSP %rcx             // %rcx dead here (last written by the mask check above)
    movq %rax, ASNIX_RETFRAME_OFF_IP(%r14)
    movq $0, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq $0, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rbx, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rbx), %rsi
    movq ASNIX_THUNK_OFF_POOL(%rbx), %r13
    movq ASNIX_THUNK_OFF_ENV(%rbx), %rdi // last read of ENV's original meaning -- safe to repurpose below
    // M82: push onto the intrusive IN_PROGRESS stack -- see
    // stacklimits.S's asnix_inprogress_head comment. %rcx is dead here
    // (last written by the ASNIX_CHECK_RETSP macro above).
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rbx)
    movq %rbx, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

asnix_force_epilogue:
    movq -8(%r12), %rax                 // result, left on VALSTACK by the thunk body's own OP_RET
    decq asnix_force_depth(%rip)        // pop -- frees this depth's slot in force_scratch.c's
                                         // per-depth cache for reuse by the NEXT call that
                                         // reaches this same depth (the buffers themselves are
                                         // never freed, only their "currently in use" status)
    popq %rdx                           // restore the enclosing context's RETSP limit (pushed
    movq %rdx, asnix_retsp_limit(%rip)  // last above, so popped first)
    popq %rdx                           // ditto for the VALSTACK limit
    movq %rdx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    movq %rax, %rdi                     // tail-loop (see .Lforce_top's comment): this
    jmp .Lforce_done                    // invocation's own frame is already fully unwound
                                         // above, so re-entering the top from here is exactly
                                         // as safe as an ordinary tail call

.Lforce_cached:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
.Lforce_done:
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lforce_check_cell
    movq %rax, %rdi                     // %rdi may be stale here (e.g. the .Lforce_cached
    jmp .Lforce_top                     // path never touches it) -- always resync from %rax

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment): the Cell's own target may itself still
    // need forcing (another Thunk), or even be another Cell (chained
    // bare-var aliasing, e.g. `let y=x; z=y;`) -- loop back through
    // .Lforce_top either way. %rcx still holds the tag bits from the
    // check just above.
.Lforce_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lforce_ret
    movq %rax, %rdx
    andq $-8, %rdx                       // rdx = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%rdx), %rcx
    cmpq $ASNIX_CELL_MARKER, %rcx
    jne .Lforce_ret                      // PTR but not a Cell (String/Path/Float): a real value

    // It's a Cell -- see op_var_ref.S's matching comment on
    // asnix_resolve_cell_chain (value_util.c). asnix_force_for_print is
    // a plain C-callable function (not a dispatch-loop op handler), so
    // its entry rsp parity is the ordinary post-`call` one (8 mod 16)
    // rather than the dispatch loop's always-16-aligned invariant --
    // .Lforce_check_cell is always reached on that same fast,
    // nothing-pushed-yet path (.Lforce_top's own entry, or
    // asnix_force_epilogue's "already fully unwound" tail-loop), so a
    // single 8-byte pad is enough to make this call ABI-compliant.
    movq %rax, %rdi                      // arg1 = the Cell value
    subq $8, %rsp
    call asnix_resolve_cell_chain
    addq $8, %rsp
    movq %rax, %rdi
    jmp .Lforce_top
.Lforce_ret:
    ret

.Lforce_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lforce_reraise:
    // M82: see op_var_ref.S's matching .Lvar_ref_reraise comment --
    // fires before this routine's own prologue pushes anything, so the
    // stack is exactly as it was on entry, same as .Lforce_blackhole
    // just above.
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_force_for_print, . - asnix_force_for_print
