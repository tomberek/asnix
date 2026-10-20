// M1: value/tagging primitives. These are the C-callable, directly testable
// boundary -- the hot evaluator core (M3+) inlines these bit tricks directly
// in op handlers rather than paying a `call` per tag check; these routines
// exist for the C side (compiler, debug printing, tests) and as the proven
// reference implementation the inlined copies must match.
#include "asnix/value.h"

    .text

    .globl asnix_make_int
    .type asnix_make_int, @function
asnix_make_int:                        // Value asnix_make_int(int64_t n)
    .cfi_startproc
    movq %rdi, %rax
    salq $ASNIX_TAG_BITS, %rax
    orq  $ASNIX_TAG_INT, %rax
    ret
    .cfi_endproc
    .size asnix_make_int, . - asnix_make_int

    .globl asnix_int_value
    .type asnix_int_value, @function
asnix_int_value:                       // int64_t asnix_int_value(Value v)
    .cfi_startproc
    movq %rdi, %rax
    sarq $ASNIX_TAG_BITS, %rax         // arithmetic shift: sign-preserving
    ret
    .cfi_endproc
    .size asnix_int_value, . - asnix_int_value

    .globl asnix_make_imm
    .type asnix_make_imm, @function
asnix_make_imm:                        // Value asnix_make_imm(int subtag)
    .cfi_startproc
    movl %edi, %eax                    // zero-extends into rax; the SysV ABI
                                        // leaves rdi's upper 32 bits undefined
                                        // for a 32-bit int argument
    salq $ASNIX_TAG_BITS, %rax
    orq  $ASNIX_TAG_IMM, %rax
    ret
    .cfi_endproc
    .size asnix_make_imm, . - asnix_make_imm

    .globl asnix_imm_subtag
    .type asnix_imm_subtag, @function
asnix_imm_subtag:                      // int asnix_imm_subtag(Value v)
    .cfi_startproc
    movq %rdi, %rax
    shrq $ASNIX_TAG_BITS, %rax         // subtag is always non-negative
    ret
    .cfi_endproc
    .size asnix_imm_subtag, . - asnix_imm_subtag

    .globl asnix_tag_of
    .type asnix_tag_of, @function
asnix_tag_of:                          // int asnix_tag_of(Value v)
    .cfi_startproc
    movq %rdi, %rax
    andq $ASNIX_TAG_MASK, %rax
    ret
    .cfi_endproc
    .size asnix_tag_of, . - asnix_tag_of

    .globl asnix_is_thunk
    .type asnix_is_thunk, @function
asnix_is_thunk:                        // int asnix_is_thunk(Value v)
    .cfi_startproc
    movq %rdi, %rax
    andq $ASNIX_TAG_MASK, %rax
    cmpq $ASNIX_TAG_THUNK, %rax
    sete %al
    movzbl %al, %eax
    ret
    .cfi_endproc
    .size asnix_is_thunk, . - asnix_is_thunk
