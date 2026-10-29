#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_attrs
    .type asnix_op_mk_attrs, @function
asnix_op_mk_attrs:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movl ASNIX_IROP_OFF_B(%rsi), %ebp
    pushq %rsi
    pushq %rdi

    movq %rbx, %rdi
    imulq $ASNIX_ATTR_SIZE, %rdi
    addq $8, %rdi
    call asnix_arena_alloc
    incq asnix_stats_attrs(%rip)
    addq %rbx, asnix_stats_attrs_entries(%rip)
    movq %rbx, ASNIX_ATTRS_OFF_COUNT(%rax)

    movq %rbx, %rcx
.Lop_mk_attrs_fill:
    testq %rcx, %rcx
    jz .Lop_mk_attrs_done
    decq %rcx
    subq $8, %r12
    movq (%r12), %rdx
    movq %rbp, %r9
    addq %rcx, %r9
    movq (%r13, %r9, 8), %r8
    movq %rcx, %r9
    imulq $ASNIX_ATTR_SIZE, %r9
    movl %r8d, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r9, 1)
    movq %rdx, ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r9, 1)
    jmp .Lop_mk_attrs_fill
.Lop_mk_attrs_done:
    movq %rax, %rdx
    orq $ASNIX_TAG_ATTRS, %rdx
    ASNIX_CHECK_VALSTACK %rcx
    movq %rdx, (%r12)
    addq $8, %r12

    popq %rdi
    popq %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_attrs, . - asnix_op_mk_attrs

