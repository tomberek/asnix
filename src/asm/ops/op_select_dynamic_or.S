// `expr.${nameExpr} or default` (M28) -- dynamic select-with-default.
// The lookup itself is deferred to C (asnix_bshared_select_dynamic_or,
// builtins_shared.c: interns the name at runtime, binary-searches,
// forces an unforced thunk on a hit), since the name isn't a compile-
// time constant the way plain OP_SELECT_OR's binary search can inline
// in asm. This handler is the SysV ABI glue (save/restore IP/ENV around
// the `call`) plus the delta-jump-on-miss dispatch, same shape as
// op_select_or.S's own OP_SELECT_OR.
//
// a = byte delta to jump to (not found / wrong type) on a miss. Pops
// the name Value then the select target (reverse push order, same as
// OP_SELECT_DYNAMIC).
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_select_dynamic_or
    .type asnix_op_select_dynamic_or, @function
asnix_op_select_dynamic_or:
    subq $16, %r12                      // pop both operands with one address computation
    movq 8(%r12), %rcx                  // name (tagged) -- pushed last, popped first
    movq (%r12), %rax                   // target (tagged)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes, 16-byte aligned)
    subq $16, %rsp                      // scratch Value out-slot (16, already a multiple of 16)
    movq %rax, %rdi                     // arg1 = target
    movq %rcx, %rsi                     // arg2 = name
    movq %rsp, %rdx                     // arg3 = &out
    call asnix_bshared_select_dynamic_or
    movq (%rsp), %rcx                   // out value (meaningful only if %eax != 0)
    addq $16, %rsp
    popq %rdi                           // ENV
    popq %rsi                           // IP (restored -- needed on both paths below)

    testl %eax, %eax
    jz .Lsdo_miss

    ASNIX_CHECK_VALSTACK %rdx           // %rdx dead (was &out)
    movq %rcx, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lsdo_miss:
    movq ASNIX_IROP_OFF_A(%rsi), %rax   // byte delta from THIS op's own address
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_select_dynamic_or, . - asnix_op_select_dynamic_or
