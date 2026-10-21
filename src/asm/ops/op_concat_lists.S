// `++` list concatenation (M23, added alongside builtins.fromTOML -- see
// ir.h's OP_CONCAT_LISTS). Same shape as op_update.S's '//' handler: the
// actual element-array copy lives in C (src/c/value_util.c,
// asnix_concat_lists), this is just the type-check + ABI glue. IP (%rsi)
// and ENV (%rdi) are NOT callee-saved by the SysV ABI, so they must be
// saved/restored around the `call` by hand (same push/pop pattern as
// op_update.S/op_mk_attrs.S); %r12/%r13/%r14 ARE callee-saved and need no
// special handling.
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_concat_type:
    .ascii "asnix: '++' operands must both be lists, aborting\n"
    .equ ASNIX_MSG_CONCAT_TYPE_LEN, . - .Lmsg_concat_type

    .text

    .globl asnix_op_concat_lists
    .type asnix_op_concat_lists, @function
asnix_op_concat_lists:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs (tagged)
    subq $8, %r12
    movq (%r12), %rax                   // lhs (tagged)

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_LIST, %r8
    jne .Lconcat_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_LIST, %r8
    jne .Lconcat_type_error

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = lhs (Value, SysV passes it by its one 8-byte word)
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_concat_lists
    popq %rdi                           // ENV (pushed last, popped first)
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lconcat_type_error:
    leaq .Lmsg_concat_type(%rip), %rdi
    movq $ASNIX_MSG_CONCAT_TYPE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_concat_lists, . - asnix_op_concat_lists
