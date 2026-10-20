// `//` attrset update/merge ("operators" milestone). The actual sorted-
// array merge lives in C (src/c/attrs_update.c, asnix_attrs_update) --
// not hot-path code, and a plain-C two-pointer merge is far easier to get
// right than hand-rolling it in asm (same rationale src/c/builtins.c
// already uses for toString/typeOf/etc). This handler is just the
// type-check + ABI glue: IP (%rsi) and ENV (%rdi) are NOT callee-saved by
// the SysV ABI, so they must be saved/restored around the `call` by hand
// (same push/pop pattern as op_mk_attrs.S's call to asnix_arena_alloc);
// %r12/%r13/%r14 ARE callee-saved and need no special handling.
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_update_type:
    .ascii "asnix: '//' operands must both be attrsets, aborting\n"
    .equ ASNIX_MSG_UPDATE_TYPE_LEN, . - .Lmsg_update_type

    .text

    .globl asnix_op_update
    .type asnix_op_update, @function
asnix_op_update:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs (tagged)
    subq $8, %r12
    movq (%r12), %rax                   // lhs (tagged)

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_ATTRS, %r8
    jne .Lupdate_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_ATTRS, %r8
    jne .Lupdate_type_error

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = lhs (Value, SysV passes it by its one 8-byte word)
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_attrs_update
    popq %rdi                           // ENV (pushed last, popped first)
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lupdate_type_error:
    leaq .Lmsg_update_type(%rip), %rdi
    movq $ASNIX_MSG_UPDATE_TYPE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_update, . - asnix_op_update
