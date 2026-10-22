// `with expr; body`: real Nix semantics -- a `with` only supplies a
// *fallback* for names that don't resolve lexically (a lexical binding
// always wins, even one introduced further out than the `with`), resolved
// at RUNTIME against the with-expression's attrset value, and lazily (the
// with-expression itself, and each enclosing with-level consulted during a
// fallback search, is only forced on demand -- see docs/architecture.md's
// `with` scope notes and the real-nix-instantiate comparisons in
// tests/e2e/m10_with_*.nix).
//
// compiler.c (AST_VAR) resolves each unbound identifier at COMPILE TIME
// into a short, bounded chain of "try this enclosing with-level" steps --
// one per lexically enclosing `with`, innermost first -- because the
// *number* of enclosing withs is a static, compile-time fact even though
// which one (if any) actually has the key is a runtime fact. Each step is:
//   OP_VAR_REF(depth_k, 0)   -- push the with-level's attrs value, forcing
//                               its thunk if unforced (ordinary OP_VAR_REF,
//                               no new code needed: a with-frame's slot 0
//                               is addressable exactly like any other
//                               lexical slot once its compile-time depth is
//                               known)
//   OP_WITH_TRY(delta, sym)  -- see below
//   OP_JMP(end)              -- only reached when OP_WITH_TRY found `sym`
//                               and fell through; jumps past the remaining
//                               levels straight to the end of the chain
// ...repeated per level, terminated by OP_WITH_MISS if every level's
// OP_WITH_TRY took its "not found" branch.
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_with_type:
    .ascii "asnix: `with' value is not an attrset, aborting\n"
    .equ ASNIX_MSG_WITH_TYPE_LEN, . - .Lmsg_with_type
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed
.Lmsg_undefined_var:
    .ascii "asnix: undefined variable, aborting\n"
    .equ ASNIX_MSG_UNDEFINED_VAR_LEN, . - .Lmsg_undefined_var

    .text

    // Binds the with-expr thunk (pushed by the OP_MK_THUNK just before this
    // op, exactly like OP_LET's binding value) into a fresh 1-slot Env
    // frame, tagging the frame's own `up` field with ASNIX_ENV_WITH_TAG so
    // OP_WITH_TRY/the var-ref depth walk can recognize it later. Mirrors
    // OP_LET's nslots=1 case exactly except for that one tag bit.
    .globl asnix_op_with
    .type asnix_op_with, @function
asnix_op_with:
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    movq $ASNIX_ENV_OFF_VALUES+8, %rdi  // size = 8 (up) + 1 slot * 8
    call asnix_arena_alloc

    movq %r15, %rcx
    orq $ASNIX_ENV_WITH_TAG, %rcx
    movq %rcx, ASNIX_ENV_OFF_UP(%rax)

    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax)

    movq %rax, %rdi                     // ENV = new with-frame
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_with, . - asnix_op_with

    // a = byte delta to jump to when `b` (the symbol id) isn't present in
    // this level's attrs; b = symbol id. Pops the attrs value a preceding
    // OP_VAR_REF already forced and pushed. Fatal if it isn't an attrset
    // (matches real Nix: a non-attrset with-value only errors once it's
    // actually consulted -- see the "with 5; 1" vs "with 5; x" comparison
    // in tests/e2e). On a hit, forces the matching entry if it's an
    // unforced thunk (same mechanism as OP_SELECT -- attrset binding
    // values are lazy) and falls through with it on top of VALSTACK;
    // compiler.c always emits an unconditional OP_JMP right after this op
    // for exactly that fallthrough to land on.
    .globl asnix_op_with_try
    .type asnix_op_with_try, @function
asnix_op_with_try:
    subq $8, %r12
    movq (%r12), %rax                   // rax = attrs value (already forced)
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lwith_try_type_error

    // M88 (lazy `//` union-layering, heap.h): resolve a UnionHdr (negative
    // `count`) to its flat form first -- see op_select.S's matching
    // comment for the full rationale.
    movq %rax, %r9                      // r9 = tagged value, kept across the maybe-call below
    andq $-8, %rax                      // rax = real Attrs* (untagged)
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = count (maybe negative)
    testq %r8, %r8
    jns .Lwith_try_resolved
    pushq %rsi
    pushq %rdi
    movq %r9, %rdi
    call asnix_bshared_attrs_resolve
    movq %rax, %r9
    popq %rdi
    popq %rsi
    movq %r9, %rax
    andq $-8, %rax
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // reload -- now guaranteed non-negative
.Lwith_try_resolved:

    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = target symbol id
    // %r8 already holds hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lwith_try_loop:
    cmpq %r8, %rcx
    jge .Lwith_try_not_found
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10         // r10 = mid * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d // zero-extends; must be
        // 32-bit -- M103 removed the pad word (see heap.h)
    cmpq %rdx, %r11
    je .Lwith_try_found
    jl .Lwith_try_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lwith_try_loop
.Lwith_try_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lwith_try_loop
.Lwith_try_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lwith_try_resolve:                     // M92: re-entered after unwrapping a Cell (see
                                         // .Lwith_try_check_cell below) -- see op_var_ref.S's
                                         // matching comment.
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lwith_try_check_cell

    andq $-8, %rax
    // M103: OFF_STATE is tagged now -- UNFORCED: this word IS the clean
    // IrProgram* (tag bits 0); otherwise: the bare state tag alone.
    // Extract into %r8 (dead since the binary search above) for
    // dispatch, keeping %rcx (the possibly-a-pointer original word)
    // intact for the UNFORCED path below.
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lwith_try_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lwith_try_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lwith_try_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    // M103: %rcx already holds the clean prog pointer (read above, tag
    // bits were 0) -- no second memory read needed (the old separate
    // OFF_PROG field is gone).
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi // last read of ENV's original meaning -- safe to repurpose below
    // M82: push onto the intrusive IN_PROGRESS stack -- see
    // stacklimits.S's asnix_inprogress_head comment. %rcx is dead here
    // (overwritten immediately below, same as before this M100 change).
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rax)
    movq %rax, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lwith_try_push                   // perf: skip the resolve re-check entirely, see
                                           // op_var_ref.S's matching .Lvar_ref_forced comment for
                                           // the invariant proof (a forced thunk's cached value can
                                           // never be a Thunk or a Cell)

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment). %rcx still holds the tag bits from the
    // check just above (the only path that reaches this label).
.Lwith_try_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lwith_try_push
    movq %rax, %r8
    andq $-8, %r8                        // r8 = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lwith_try_push                  // PTR but not a Cell (String/Path/Float): a real value

    // It's a Cell -- see op_var_ref.S's matching comment on
    // asnix_resolve_cell_chain (value_util.c).
    pushq %rsi                           // save IP
    pushq %rdi                           // save ENV (2 pushes, stays 16-byte aligned)
    movq %rax, %rdi                      // arg1 = the Cell value
    call asnix_resolve_cell_chain
    popq %rdi                            // restore ENV
    popq %rsi                            // restore IP
    jmp .Lwith_try_resolve
.Lwith_try_push:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_not_found:
    movq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_type_error:
    leaq .Lmsg_with_type(%rip), %rdi
    movq $ASNIX_MSG_WITH_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lwith_try_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
.Lwith_try_reraise:
    // M82: see op_var_ref.S's matching .Lvar_ref_reraise comment.
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_with_try, . - asnix_op_with_try

    // All enclosing `with`s were tried (every OP_WITH_TRY took its
    // "not found" branch) -- same error a compile-time-unbound variable
    // gets (see compiler.c's AST_VAR case), just raised at runtime since
    // whether a `with`'s attrset actually has the key is a runtime fact.
    .globl asnix_op_with_miss
    .type asnix_op_with_miss, @function
asnix_op_with_miss:
    leaq .Lmsg_undefined_var(%rip), %rdi
    movq $ASNIX_MSG_UNDEFINED_VAR_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_with_miss, . - asnix_op_with_miss

    // M52: no operands; ENV = ENV->up with ASNIX_ENV_WITH_TAG masked off --
    // the with-frame counterpart of OP_FRAME_EXIT (op_frame.S). compiler.c's
    // AST_WITH case emits this right after compiling the `with` body so a
    // non-tail `with` (e.g. one sibling among several in the same list/
    // attrset/call-arg position) doesn't leave its 1-slot with-frame
    // installed as ENV for whatever compiles next -- that leak was M52's
    // bug: a second, sibling `with`'s OP_MK_THUNK (building its own
    // with-expr) would capture the first with's leftover frame instead of
    // the enclosing scope, so every depth-relative OP_VAR_REF inside that
    // thunk walked one frame too many once the second `with`'s value was
    // actually forced. Without the mask, ENV would become a tagged (odd)
    // pointer -- a plain OP_FRAME_EXIT, whose frames are never tagged,
    // can't be reused here.
    .globl asnix_op_with_exit
    .type asnix_op_with_exit, @function
asnix_op_with_exit:
    movq ASNIX_ENV_OFF_UP(%rdi), %rdi
    andq $-2, %rdi                       // mask off ASNIX_ENV_WITH_TAG (bit 0)

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_with_exit, . - asnix_op_with_exit
