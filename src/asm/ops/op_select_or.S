// `expr.attr or default` -- real Nix's select-with-default. Modeled
// directly on op_with_try.S's OP_WITH_TRY (same binary-search + delta-jump
// shape) with one behavioral difference: a non-attrset target is NOT
// fatal here (confirmed against real nix-instantiate: `(5).foo or 10`
// => 10, same "not even an attrset simply means not found" rule
// op_has_attr.S's OP_HAS_ATTR already established for `?`), whereas
// OP_WITH_TRY's non-attrset `with`-value IS fatal (a `with` value is only
// ever consulted once something inside actually needs it, so by the time
// OP_WITH_TRY runs, the type error is real and immediate).
//
// a = byte delta to jump to (not found / wrong type); b = symbol id.
// Pops the select target off VALSTACK. On a hit: forces the matching
// entry if it's an unforced thunk (same re-entrant mechanism as
// OP_SELECT/OP_WITH_TRY -- attrset binding values are lazy) and falls
// through with it on top of VALSTACK, landing past the compiler-emitted
// default expression's IR (compiler.c's AST_SELECT case, when a default
// is present, always emits the default's code followed by the point this
// falls through to -- no separate OP_JMP needed here, unlike OP_WITH_TRY,
// since there's only one "level" to try, not a chain).
//
// Scope cut (documented, see docs/architecture.md): this only protects
// the single select this op corresponds to. `a.b.c or 5` compiles as
// nested selects (see compiler.c's AST_SELECT), and this op is only used
// for the OUTERMOST (last) one -- an earlier segment missing (e.g. `.b`
// not existing on `a`) is still a fatal plain OP_SELECT abort, not caught
// by the trailing `or`. Real Nix's `or` catches a missing/wrong-type
// value at ANY point along a multi-segment path; asnix's current scope
// only covers the last segment.
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

    .globl asnix_op_select_or
    .type asnix_op_select_or, @function
asnix_op_select_or:
    subq $8, %r12
    movq (%r12), %rax                   // rax = target value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lselect_or_not_found           // not even an attrset -- not fatal, see header comment
    andq $-8, %rax                      // rax = real Attrs*

    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = target symbol id
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lselect_or_loop:
    cmpq %r8, %rcx
    jge .Lselect_or_not_found
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    shlq $4, %r10                       // r10 = mid * ASNIX_ATTR_SIZE
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11
    cmpq %rdx, %r11
    je .Lselect_or_found
    jl .Lselect_or_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lselect_or_loop
.Lselect_or_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lselect_or_loop
.Lselect_or_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lselect_or_resolve:                    // M92: re-entered after unwrapping a Cell (see
                                         // .Lselect_or_check_cell below) -- see op_var_ref.S's
                                         // matching comment.
    // M6: force if the selected entry holds an unforced thunk, same as
    // OP_SELECT.
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lselect_or_check_cell

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lselect_or_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lselect_or_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %rcx
    je .Lselect_or_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    ASNIX_CHECK_RETSP %rcx              // %rcx dead since the binary search above
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rax), %rsi
    movq ASNIX_THUNK_OFF_POOL(%rax), %r13
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi // last read of ENV's original meaning -- safe to repurpose below
    // M82: push onto the intrusive IN_PROGRESS stack -- see
    // stacklimits.S's asnix_inprogress_head comment. %rcx is dead here
    // (last written by the STATE check above).
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rax)
    movq %rax, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

.Lselect_or_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lselect_or_resolve               // M92: defensive re-check, see op_var_ref.S's matching comment

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment). %rcx still holds the tag bits from the
    // check just above (the only path that reaches this label).
.Lselect_or_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_or_push
    movq %rax, %r8
    andq $-8, %r8                        // r8 = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_or_push                 // PTR but not a Cell (String/Path/Float): a real value

    // It's a Cell -- see op_var_ref.S's matching comment on
    // asnix_resolve_cell_chain (value_util.c).
    pushq %rsi                           // save IP
    pushq %rdi                           // save ENV (2 pushes, stays 16-byte aligned)
    movq %rax, %rdi                      // arg1 = the Cell value
    call asnix_resolve_cell_chain
    popq %rdi                            // restore ENV
    popq %rsi                            // restore IP
    jmp .Lselect_or_resolve
.Lselect_or_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_or_not_found:
    movq ASNIX_IROP_OFF_A(%rsi), %rax   // byte delta from THIS op's own address
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_or_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal

.Lselect_or_reraise:
    // M82: see op_var_ref.S's matching .Lvar_ref_reraise comment.
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal
    .size asnix_op_select_or, . - asnix_op_select_or
