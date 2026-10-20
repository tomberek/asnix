#include "asnix/heap.h"
#include "asnix/ir.h"
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
    shlq $4, %rdi                       // rdi = count * ASNIX_ATTR_SIZE
    addq $8, %rdi                       // rdi = 8 + count*16 (header + entries)
    call asnix_arena_alloc
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
    movq (%r13, %r9, 8), %r8            // r8 = symbol id for entries[i] (zero-extended; pad is 0)
    movq %rcx, %r9
    shlq $4, %r9                        // r9 = i * ASNIX_ATTR_SIZE
    movq %r8, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r9, 1)
    movq %rdx, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r9, 1)
    jmp .Lop_mk_attrs_fill
.Lop_mk_attrs_done:
    movq %rax, %rdx
    orq $ASNIX_TAG_ATTRS, %rdx
    movq %rdx, (%r12)
    addq $8, %r12

    popq %rdi                           // ENV (pushed last, popped first)
    popq %rsi                           // IP
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_attrs, . - asnix_op_mk_attrs
