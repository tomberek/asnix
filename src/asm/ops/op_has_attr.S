#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

// `expr ? attr` / lambda-pattern default checks: never a fatal error, unlike
// op_select.S's OP_SELECT -- a non-attrset left-hand side simply means "no"
// (confirmed against real `nix-instantiate --eval '1 ? a'` => false), and a
// present-but-unforced entry is never forced: this is a membership test on
// symbol ids only, not a read of the value.

    .text

    .globl asnix_op_has_attr
    .type asnix_op_has_attr, @function
asnix_op_has_attr:                      // a = symbol id; pops a value, pushes TRUE/FALSE
    subq $8, %r12
    movq (%r12), %rax                   // rax = tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lhas_attr_false

    // M88 (lazy `//` union-layering, heap.h): a UnionHdr has a NEGATIVE
    // `count` at offset 0 -- a single sign-bit check (no call overhead
    // paid) on the common already-flat case.
    //
    // M154 (unionhdr-probe, generalizing M111's op_select.S-only fork):
    // the union case used to call asnix_bshared_attrs_resolve, which
    // ALWAYS fully flattens + caches every leaf merged together before
    // this function's own single-key binary search ever runs, even
    // though `?` is a pure membership test that never needs the found
    // value or a positional index. Now calls asnix_bshared_attrs_probe
    // instead -- see op_select.S's matching comment and
    // asnix_bshared_attrs_probe's own comment (builtins_shared.c) for
    // the full reasoning. The probed value itself is discarded (`?`
    // never forces or returns it, matching the pre-existing behavior
    // above -- a present-but-unforced entry still counts as present).
    movq %rax, %r9                      // r9 = tagged value, kept across the maybe-call below
    andq $-8, %rax                      // rax = Attrs* (untagged)
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = count (maybe negative -- union header)
    testq %r8, %r8
    jns .Lhas_attr_resolved
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    subq $16, %rsp                      // 16-byte-aligned scratch slot for out_value (unused
                                         // by the caller, but asnix_bshared_attrs_probe always
                                         // writes it on a hit)
    movq %r9, %rdi                      // arg1 = tagged attrs value
    movl ASNIX_IROP_OFF_A(%rsi), %esi   // arg2 = symbol id (read before clobbering %rsi below)
    movq %rsp, %rdx                     // arg3 = &out_value (discarded)
    call asnix_bshared_attrs_probe
    addq $16, %rsp
    popq %rdi
    popq %rsi
    // M154: `testl %eax, %eax` MUST be the LAST thing before `jnz` -- see
    // op_select.S's matching comment for the real bug this ordering
    // avoids (an intervening `addq $16, %rsp` between the test and the
    // branch silently clobbers the flags `testl` just set, found by
    // direct repro on a sibling file during this milestone).
    testl %eax, %eax
    jnz .Lhas_attr_true
    jmp .Lhas_attr_false
.Lhas_attr_resolved:

    movl ASNIX_IROP_OFF_A(%rsi), %edx   // rdx = target symbol id
    // %r8 already holds hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lhas_attr_loop:
    cmpq %r8, %rcx
    jge .Lhas_attr_false
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10         // r10 = mid * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d // zero-extends; must be
        // 32-bit -- M103 removed the pad word (see heap.h)
    cmpq %rdx, %r11
    je .Lhas_attr_true
    jl .Lhas_attr_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lhas_attr_loop
.Lhas_attr_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lhas_attr_loop

.Lhas_attr_true:
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jmp .Lhas_attr_push
.Lhas_attr_false:
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
.Lhas_attr_push:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr, . - asnix_op_has_attr
