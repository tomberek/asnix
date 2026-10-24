// `expr ? ${nameExpr}` (M33) -- dynamic has-attr check. The lookup itself
// is deferred to C (asnix_bshared_has_attr_dynamic, builtins_shared.c:
// interns the name at runtime, binary-searches), since the name isn't a
// compile-time constant the way plain OP_HAS_ATTR's binary search can
// inline in asm. This handler is just the SysV ABI glue (save/restore
// IP/ENV around the `call`) plus pushing the TRUE/FALSE result -- no miss-
// jump needed, unlike op_select_dynamic_or.S, since `?` never has a
// separate "default" branch to fall through to.
//
// Pops the name Value then the select target (reverse push order, same
// convention as OP_SELECT_DYNAMIC/OP_SELECT_DYNAMIC_OR).
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_has_attr_dynamic
    .type asnix_op_has_attr_dynamic, @function
asnix_op_has_attr_dynamic:
    subq $16, %r12                      // pop both operands with one address computation
    movq 8(%r12), %rcx                  // name (tagged) -- pushed last, popped first
    movq (%r12), %rax                   // target (tagged)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes, 16-byte aligned)
    movq %rax, %rdi                     // arg1 = target
    movq %rcx, %rsi                     // arg2 = name
    call asnix_bshared_has_attr_dynamic
    popq %rdi                           // ENV
    popq %rsi                           // IP

    testl %eax, %eax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rcx
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovzq %rdx, %rcx
    movq %rcx, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr_dynamic, . - asnix_op_has_attr_dynamic
