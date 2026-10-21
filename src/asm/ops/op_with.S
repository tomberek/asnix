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
    andq $-8, %rax                      // rax = real Attrs*

    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = target symbol id
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lwith_try_loop:
    cmpq %r8, %rcx
    jge .Lwith_try_not_found
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    shlq $4, %r10                       // r10 = mid * ASNIX_ATTR_SIZE
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11
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

    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lwith_try_push

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lwith_try_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lwith_try_blackhole

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rax), %rsi
    movq ASNIX_THUNK_OFF_POOL(%rax), %r13
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi
    movq (%rsi), %rax
    jmp *%rax

.Lwith_try_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
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
