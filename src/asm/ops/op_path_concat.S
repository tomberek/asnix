#include "asnix/ir.h"
#include "asnix/value.h"

    .text

/* M59: identical to op_str_concat.S except for the C helper called --
 * see OP_PATH_CONCAT's comment in ir.h for why asnix_concat_path (not
 * asnix_concat_strings) is safe to call unconditionally here. */
    .globl asnix_op_path_concat
    .type asnix_op_path_concat, @function
asnix_op_path_concat:                   // a = count; pops `count` values (reverse push order,
                                         // same convention as OP_STR_CONCAT), concatenates
                                         // them and re-canonicalizes the result as a Path.
    movl ASNIX_IROP_OFF_A(%rsi), %ebx   // rbx = count (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save ENV (ditto)

    movq %rbx, %rdx
    shlq $3, %rdx                       // rdx = count * 8
    movq %r12, %rdi
    subq %rdx, %rdi                     // rdi = base ptr = VALSTACK top - count*8 (asnix_concat_path's `parts`)
    movq %rbx, %rsi                     // rsi = count
    call asnix_concat_path               // rax = result Value (already tagged ASNIX_TAG_PTR)

    movq %rbx, %rdx
    shlq $3, %rdx
    subq %rdx, %r12                     // pop `count` slots
    movq %rax, (%r12)                   // push result
    addq $8, %r12

    movq %r15, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_path_concat, . - asnix_op_path_concat
