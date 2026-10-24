// M162: `if (expr ? attr) then ... else ...` -- compiler.c's AST_IF case
// emits this instead of {OP_HAS_ATTR; OP_IF} whenever the `if`'s own
// condition is LITERALLY a bare `?` test. Pops a value and branches
// directly off the SAME membership test op_has_attr.S's OP_HAS_ATTR
// computes, without ever tagging/pushing/popping/re-deriving a TRUE/FALSE
// Value for OP_IF to immediately re-validate -- see ir.h's own comment on
// this op for the full rationale. `a` = byte delta to the else-branch
// (self-relative, same convention as OP_IF -- patched via
// ib_patch_delta), taken when the attr is absent; `b` = symbol id (same
// a/b roles as OP_WITH_TRY/OP_SELECT_OR). A present attr falls through to
// the very next op (the compiled "then" branch).
//
// Every behavioral nuance of plain OP_HAS_ATTR is preserved exactly: a
// non-attrset target is NOT fatal (just takes the else-branch, matching
// `expr ? attr` => false for a non-attrset `expr`), and a present-but-
// unforced entry is never forced (membership-only, never a read of the
// value) -- see op_has_attr.S's own header comment for why.
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_has_attr_if
    .type asnix_op_has_attr_if, @function
asnix_op_has_attr_if:                   // a = byte delta to else; b = symbol id
    subq $8, %r12
    movq (%r12), %rax                   // rax = tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lhas_attr_if_false

    // M88/M154 (see op_has_attr.S's matching comment): a UnionHdr has a
    // NEGATIVE `count` at offset 0 -- resolve via asnix_bshared_attrs_probe
    // (a pure membership probe, never fully flattening) on that path.
    movq %rax, %r9                      // r9 = tagged value, kept across the maybe-call below
    andq $-8, %rax                      // rax = Attrs* (untagged)
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = count (maybe negative -- union header)
    testq %r8, %r8
    jns .Lhas_attr_if_resolved
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    subq $16, %rsp                      // 16-byte-aligned scratch slot for out_value (unused
                                         // by the caller, but asnix_bshared_attrs_probe always
                                         // writes it on a hit)
    movq %r9, %rdi                      // arg1 = tagged attrs value
    movl ASNIX_IROP_OFF_B(%rsi), %esi   // arg2 = symbol id (read before clobbering %rsi below)
    movq %rsp, %rdx                     // arg3 = &out_value (discarded)
    call asnix_bshared_attrs_probe
    addq $16, %rsp
    popq %rdi
    popq %rsi
    // M154 (op_has_attr.S's matching comment): `testl %eax, %eax` MUST be
    // the LAST thing before the branch -- an intervening `addq` between
    // the test and the branch would silently clobber the flags.
    testl %eax, %eax
    jnz .Lhas_attr_if_true
    jmp .Lhas_attr_if_false
.Lhas_attr_if_resolved:

    movl ASNIX_IROP_OFF_B(%rsi), %edx   // rdx = target symbol id
    // %r8 already holds hi = count
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lhas_attr_if_loop:
    cmpq %r8, %rcx
    jge .Lhas_attr_if_false
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10         // r10 = mid * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d // zero-extends; must be
        // 32-bit -- M103 removed the pad word (see heap.h)
    cmpq %rdx, %r11
    je .Lhas_attr_if_true
    jl .Lhas_attr_if_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lhas_attr_if_loop
.Lhas_attr_if_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lhas_attr_if_loop

.Lhas_attr_if_true:                     // present -- fall through to the compiled "then" branch
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lhas_attr_if_false:                    // absent / not an attrset -- jump to the "else" branch
    movslq ASNIX_IROP_OFF_A(%rsi), %rax // byte delta from THIS op's own address
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr_if, . - asnix_op_has_attr_if
