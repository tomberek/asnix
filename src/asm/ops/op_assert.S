#include "asnix/ir.h"

    .text

    .globl asnix_op_assert_fail
    .type asnix_op_assert_fail, @function
asnix_op_assert_fail:
    movl ASNIX_IROP_OFF_B(%rsi), %eax
    movq (%r13, %rax, 8), %rdi
    movq 8(%r13, %rax, 8), %rsi
    call asnix_tryeval_raise_or_fatal
    .size asnix_op_assert_fail, . - asnix_op_assert_fail

