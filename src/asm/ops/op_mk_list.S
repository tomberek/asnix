#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_mk_list
    .type asnix_op_mk_list, @function
asnix_op_mk_list:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movq %rsi, %rbp
    movq %rdi, %r15

    leaq 8(, %rbx, 8), %rdi
    call asnix_arena_alloc
    incq asnix_stats_list(%rip)
    addq %rbx, asnix_stats_list_elements(%rip)
    movq %rbx, ASNIX_LIST_OFF_COUNT(%rax)

    movq %rbx, %rcx
.Lop_mk_list_fill:
    testq %rcx, %rcx
    jz .Lop_mk_list_done
    decq %rcx
    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_LIST_OFF_ELEMS(%rax, %rcx, 8)
    jmp .Lop_mk_list_fill
.Lop_mk_list_done:
    movq %rax, %rdx
    orq $ASNIX_TAG_LIST, %rdx
    ASNIX_CHECK_VALSTACK %rcx
    movq %rdx, (%r12)
    addq $8, %r12

    movq %r15, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mk_list, . - asnix_op_mk_list

