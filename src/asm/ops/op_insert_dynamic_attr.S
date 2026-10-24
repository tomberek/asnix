// `${nameExpr} = value;` inside a plain `{...}`/`rec {...}` literal
// (M28). The actual intern/binary-search/insert logic lives in C
// (src/c/attrs_update.c, asnix_attrs_insert_dynamic) -- not hot-path code,
// same rationale as OP_UPDATE/OP_CONCAT. This handler is just the SysV
// ABI glue: IP (%rsi) and ENV (%rdi) are NOT callee-saved, so they must
// be saved/restored around the `call` by hand, same push/pop pattern as
// op_update.S/op_concat.S.
//
// Pops value, then name, then attrs (reverse push order -- compiler.c's
// AST_ATTRSET case pushes the already-built static attrs first, then
// the dynamic name, then the value thunk).
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_insert_dynamic_attr
    .type asnix_op_insert_dynamic_attr, @function
asnix_op_insert_dynamic_attr:
    subq $24, %r12                      // pop all three operands with one address computation
    movq 16(%r12), %rcx                 // value (tagged) -- pushed last, popped first
    movq 8(%r12), %rdx                  // name (tagged)
    movq (%r12), %rax                   // attrs (tagged) -- pushed first

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = attrs
    movq %rdx, %rsi                     // arg2 = name
    movq %rcx, %rdx                     // arg3 = value
    call asnix_attrs_insert_dynamic
    popq %rdi                           // ENV
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_insert_dynamic_attr, . - asnix_op_insert_dynamic_attr
