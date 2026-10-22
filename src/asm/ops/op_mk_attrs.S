#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_attrs
    .type asnix_op_mk_attrs, @function
asnix_op_mk_attrs:                      // a = count, b = pool index of `count` sorted symbol ids
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = count (callee-saved, survives the call below)
    movq ASNIX_IROP_OFF_B(%rsi), %rbp   // rbp = pool_idx (ditto)
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)

    movq %rbx, %rdi
    imulq $ASNIX_ATTR_SIZE, %rdi        // rdi = count * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    addq $8, %rdi                       // rdi = 8 + count*12 (header + entries)
    call asnix_arena_alloc
    incq asnix_stats_attrs(%rip)
    addq %rbx, asnix_stats_attrs_entries(%rip)
    movq %rbx, ASNIX_ATTRS_OFF_COUNT(%rax)

    // Fill entries[count-1 .. 0], matching the compiler's convention of
    // pushing binding values in ascending-symbol order (so popping from
    // the top, in reverse, lands each value at its correct sorted index).
    movq %rbx, %rcx                     // rcx = i, counting down from count
.Lop_mk_attrs_fill:
    testq %rcx, %rcx
    jz .Lop_mk_attrs_done
    decq %rcx
    subq $8, %r12
    movq (%r12), %rdx                   // rdx = value for entries[i]
    movq %rbp, %r9
    addq %rcx, %r9
    movq (%r13, %r9, 8), %r8            // r8 = symbol id for entries[i] (compiler constant pool
                                         // stores it as a plain non-negative int64_t)
    movq %rcx, %r9
    imulq $ASNIX_ATTR_SIZE, %r9         // r9 = i * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl %r8d, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r9, 1) // MUST be 32-bit: M103
        // removed the pad word, so this entry's value bytes start immediately after sym -- a movq
        // here would zero them, about to be overwritten correctly below anyway, but write the
        // narrow form regardless rather than relying on write-order to paper over it
    movq %rdx, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r9, 1)
    jmp .Lop_mk_attrs_fill
.Lop_mk_attrs_done:
    movq %rax, %rdx
    orq $ASNIX_TAG_ATTRS, %rdx
    // Net effect of this op is a pop (count values replaced by 1 attrset),
    // so this push can never actually exceed the cap -- checked anyway,
    // same rationale as op_mk_list.S. %rcx == 0 here (fill loop above
    // counts it down to zero).
    ASNIX_CHECK_VALSTACK %rcx
    movq %rdx, (%r12)
    addq $8, %r12

    popq %rdi                           // ENV (pushed last, popped first)
    popq %rsi                           // IP
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_attrs, . - asnix_op_mk_attrs
