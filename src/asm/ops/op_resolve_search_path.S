// `<name>` search-path resolution (M31). Deferred entirely to C
// (asnix_resolve_search_path_value, compiler.c) -- not a hot path, and
// the actual NIX_PATH-colon-list-scanning logic is far easier to get
// right in plain C than hand-rolled in asm, same rationale as every
// other "glue is asm, the real logic is a plain C call" op in this
// project (OP_UPDATE, OP_CONCAT, ...). Pops the search name (a String,
// pushed as an ordinary compile-time constant by compiler.c's AST_PATH
// case), pushes the resolved Path -- or the C call never returns at all
// (asnix_tryeval_raise_or_fatal, reached on a lookup miss), in which case
// none of this handler's own code after the `call` ever runs.
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_resolve_search_path
    .type asnix_op_resolve_search_path, @function
asnix_op_resolve_search_path:
    subq $8, %r12
    movq (%r12), %rax                   // name (tagged String)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes, 16-byte aligned)
    movq %rax, %rdi                     // arg1 = name
    call asnix_resolve_search_path_value
    popq %rdi                           // ENV
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_resolve_search_path, . - asnix_op_resolve_search_path
