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

    .globl asnix_op_select
    .type asnix_op_select, @function
asnix_op_select:                        // a = symbol id; pops an attrset, pushes the matching value
    subq $8, %r12
    movq (%r12), %rax                   // rax = attrs tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lselect_type_error

    // M88 (lazy `//` union-layering, heap.h): a UnionHdr has a NEGATIVE
    // `count` at offset 0 -- resolve to the flat form before the binary
    // search below ever reads raw layout. asnix_bshared_attrs_resolve is
    // a single sign-bit check (no call overhead paid) on the common
    // already-flat case, which is why this is checked inline here rather
    // than always calling through.
    movq %rax, %r9                      // r9 = tagged value, kept across the maybe-call below
    andq $-8, %rax                      // rax = real Attrs* (untagged)
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = count (maybe negative -- union header)
    testq %r8, %r8
    jns .Lselect_resolved
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %r9, %rdi                      // arg1 = tagged attrs value
    call asnix_bshared_attrs_resolve
    movq %rax, %r9                      // r9 = resolved (flat) tagged value
    popq %rdi
    popq %rsi
    movq %r9, %rax
    andq $-8, %rax                      // rax = real (flat) Attrs*
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // reload count -- now guaranteed non-negative
.Lselect_resolved:

    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = target symbol id
    // %r8 already holds hi = count (loaded above)
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lselect_loop:
    cmpq %r8, %rcx
    jge .Lselect_missing
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10         // r10 = mid * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d // zero-extends into %r11;
        // MUST be 32-bit: M103 removed the pad word, so sym (4 bytes) is
        // immediately followed by this same entry's value bytes -- a movq
        // here would read into them
    cmpq %rdx, %r11
    je .Lselect_found
    jl .Lselect_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lselect_loop
.Lselect_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lselect_loop
.Lselect_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lselect_resolve:                       // M92: re-entered after unwrapping a Cell (see
                                         // .Lselect_check_cell below) -- see op_var_ref.S's
                                         // matching comment.
    // M6: force if the selected entry holds an unforced thunk (laziness
    // -- attrset binding values are deferred the same way `let` bindings
    // are; see op_mk_thunk.S / compiler.c).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lselect_check_cell

    andq $-8, %rax
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lselect_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lselect_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %rcx
    je .Lselect_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    ASNIX_CHECK_RETSP %rcx              // %rcx dead since the binary search above
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    // M100: prog (one pointer) replaces {body,pool} -- %rcx is free here
    // (last written by the STATE check above; reused again right below
    // for the IN_PROGRESS-stack push, after this).
    movq ASNIX_THUNK_OFF_PROG(%rax), %rcx
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

.Lselect_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lselect_push                    // perf: skip the resolve re-check entirely, see
                                          // op_var_ref.S's matching .Lvar_ref_forced comment for
                                          // the invariant proof (a forced thunk's cached value can
                                          // never be a Thunk or a Cell)

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment). %rcx still holds the tag bits from the
    // check just above (the only path that reaches this label).
.Lselect_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_push
    movq %rax, %r8
    andq $-8, %r8                        // r8 = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_push                    // PTR but not a Cell (String/Path/Float): a real value

    // It's a Cell -- see op_var_ref.S's matching comment on
    // asnix_resolve_cell_chain (value_util.c).
    pushq %rsi                           // save IP
    pushq %rdi                           // save ENV (2 pushes, stays 16-byte aligned)
    movq %rax, %rdi                      // arg1 = the Cell value
    call asnix_resolve_cell_chain
    popq %rdi                            // restore ENV
    popq %rsi                            // restore IP
    jmp .Lselect_resolve
.Lselect_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_reraise:
    // M82: see op_var_ref.S's matching .Lvar_ref_reraise comment --
    // re-forcing a previously-FAILED thunk re-raises instead of re-
    // running the body or blackholing.
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal

.Lselect_type_error:
    // M50: names the actual type of the non-attrset target (real `call`
    // into C, not a tail `jmp` -- same reasoning as .Lselect_missing
    // below/M36: %rsp is already 16-byte aligned entering this handler,
    // asnix_bshared_select_type_fatal is _Noreturn so the pushed return
    // address is never used, and this only ever runs immediately before
    // exit(1), so it costs nothing on the hot (successful select) path
    // above. %rax still holds the original tagged Value here (loaded at
    // handler entry, not yet `andq $-8`'d).
    movq %rax, %rdi
    call asnix_bshared_select_type_fatal
.Lselect_missing:
    // Names the attribute (real Nix's own "attribute 'x' missing"
    // wording) -- a real `call` into C (not a tail `jmp` like the other
    // two fatal paths below), safe here because %rsp is already 16-byte
    // aligned entering this handler (same standing dispatch-loop
    // invariant op_call.S/op_mk_thunk.S's own direct C calls rely on) and
    // asnix_bshared_select_missing_fatal is _Noreturn, so the harmless
    // pushed return address is never used. Only reached immediately
    // before exit(1), so this costs nothing on the hot (successful
    // select) path above.
    movq %rdx, %rdi
    call asnix_bshared_select_missing_fatal
.Lselect_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_select, . - asnix_op_select
