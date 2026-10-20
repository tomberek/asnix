#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole

    .text

    .globl asnix_op_var_ref
    .type asnix_op_var_ref, @function
asnix_op_var_ref:                       // a = depth, b = slot
    movq %rdi, %rcx                     // rcx = env, walked up `depth` hops
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = depth
.Lvar_ref_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_done
    movq ASNIX_ENV_OFF_UP(%rcx), %rcx
    decq %rdx
    jmp .Lvar_ref_walk
.Lvar_ref_done:
    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = slot
    movq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rax // rax = raw tagged value

    // M6: force if this slot holds an unforced thunk (laziness).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lvar_ref_push

    andq $-8, %rax                      // rax = Thunk*
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lvar_ref_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lvar_ref_blackhole

    // UNFORCED: enter the thunk's body like a zero-arg call, with a
    // retframe that tells OP_RET to memoize the result back into this
    // cell instead of just resuming (see op_call.S).
    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx         // return_ip = one past this VAR_REF record
    ASNIX_CHECK_RETSP %rcx              // %rcx dead since the depth-walk loop above
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)     // ENV/POOL are unchanged by var_ref itself
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rax), %rsi       // IP = thunk body (read before ENV overwrites rdi, in case body==env aliasing ever mattered -- it doesn't here, but keep the read order defensive)
    movq ASNIX_THUNK_OFF_POOL(%rax), %r13
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi         // ENV = thunk's captured scope
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax // cached Value.raw (ENV slot repurposed once forced)
.Lvar_ref_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lvar_ref_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_var_ref, . - asnix_op_var_ref
