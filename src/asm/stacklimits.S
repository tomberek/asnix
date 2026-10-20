// Shared state and abort routines backing include/asnix/stackcheck.h's
// ASNIX_CHECK_VALSTACK/ASNIX_CHECK_RETSP macros, used at every VALSTACK/
// RETSP push site (grep ASNIX_CHECK_VALSTACK / ASNIX_CHECK_RETSP). Both
// dispatch.S's top-level eval loop and force.S's smaller nested-forcing
// context write asnix_valstack_limit/asnix_retsp_limit to "one past the
// last valid byte" of whichever buffer they just allocated (force.S saves
// and restores the previous values around its own nested use, exactly
// like it already does for %r12/%r14 -- see force.S) so the same shared
// op handler code enforces the right cap in either context. Exceeding
// either cap aborts cleanly via asnix_fatal, matching the
// ASNIX_MAX_MEMORY philosophy in arena.S, instead of silently corrupting
// whatever the arena allocates next.
    .bss
    .align 8
    .globl asnix_valstack_limit
    .type asnix_valstack_limit, @object
asnix_valstack_limit:
    .quad 0
    .size asnix_valstack_limit, 8

    .align 8
    .globl asnix_retsp_limit
    .type asnix_retsp_limit, @object
asnix_retsp_limit:
    .quad 0
    .size asnix_retsp_limit, 8

    .section .rodata
.Lmsg_valstack_overflow:
    .ascii "asnix: value stack overflow (too many live pushed values), aborting\n"
    .equ ASNIX_MSG_VALSTACK_OVERFLOW_LEN, . - .Lmsg_valstack_overflow
.Lmsg_retsp_overflow:
    .ascii "asnix: call/force stack overflow (too much nested recursion), aborting\n"
    .equ ASNIX_MSG_RETSP_OVERFLOW_LEN, . - .Lmsg_retsp_overflow

    .text

    .globl asnix_valstack_overflow
    .type asnix_valstack_overflow, @function
asnix_valstack_overflow:                // never returns
    leaq .Lmsg_valstack_overflow(%rip), %rdi
    movq $ASNIX_MSG_VALSTACK_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_valstack_overflow, . - asnix_valstack_overflow

    .globl asnix_retsp_overflow
    .type asnix_retsp_overflow, @function
asnix_retsp_overflow:                   // never returns
    leaq .Lmsg_retsp_overflow(%rip), %rdi
    movq $ASNIX_MSG_RETSP_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_retsp_overflow, . - asnix_retsp_overflow
