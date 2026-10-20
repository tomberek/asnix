// `++` list concatenation ("M23": see docs/architecture.md). The actual
// concatenation lives in C (src/c/value_util.c, asnix_concat_lists) --
// not hot-path code, and a plain-C two-memcpy build is far easier to get
// right than hand-rolling it in asm, same rationale op_update.S already
// uses for `//`. This handler is just the ABI glue: IP (%rsi) and ENV
// (%rdi) are NOT callee-saved by the SysV ABI, so they must be saved/
// restored around the `call` by hand (same push/pop pattern as
// op_update.S/op_mk_attrs.S's calls into C).
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_concat
    .type asnix_op_concat, @function
asnix_op_concat:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs (tagged)
    subq $8, %r12
    movq (%r12), %rax                   // lhs (tagged)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = lhs (Value, SysV passes it by its one 8-byte word)
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_concat_lists             // fatal (exits) on a non-list operand, never returns in that case
    popq %rdi                           // ENV (pushed last, popped first)
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_concat, . - asnix_op_concat
