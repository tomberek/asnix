#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_call_type:
    .ascii "asnix: call target is not a function, aborting\n"
    .equ ASNIX_MSG_CALL_TYPE_LEN, . - .Lmsg_call_type
.Lmsg_call_arity:
    .ascii "asnix: call argument count does not match arity, aborting\n"
    .equ ASNIX_MSG_CALL_ARITY_LEN, . - .Lmsg_call_arity

    .text

    .globl asnix_op_call
    .type asnix_op_call, @function
asnix_op_call:                          // a = argc
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = argc (callee-saved, survives the call below)

    // Locate the callee: VALSTACK[top - argc - 1] (args were pushed after it).
    movq %rbx, %rax
    incq %rax
    shlq $3, %rax
    movq %r12, %rcx
    subq %rax, %rcx                     // rcx = address of the callee's slot
    movq (%rcx), %rdx                   // rdx = callee Value (tagged)

    movq %rdx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_CLOSURE, %r8
    jne .Lop_call_type_error
    andq $-8, %rdx                      // rdx = real Closure* (arena pointers are 8-byte aligned)

    movq ASNIX_CLOSURE_OFF_ARITY(%rdx), %r8
    cmpq %r8, %rbx
    jne .Lop_call_arity_error

    movq %rdx, %rbp                     // rbp = closure ptr (callee-saved, survives the call below)
    movq %rcx, %r15                     // r15 = callee slot address (callee-saved, survives the call below)

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV == the return_env (2 pushes, stays 16-byte aligned)

    movq %rbx, %rcx
    leaq 8(, %rcx, 8), %rdi             // size = 8 + argc*8
    call asnix_arena_alloc
    movq ASNIX_CLOSURE_OFF_UP(%rbp), %rcx
    movq %rcx, ASNIX_ENV_OFF_UP(%rax)

    xorq %rcx, %rcx                     // rcx = i = 0
.Lop_call_copy_args:
    cmpq %rbx, %rcx
    jge .Lop_call_copy_done
    movq 8(%r15, %rcx, 8), %rdx         // arg_i, still sitting where it was pushed
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    incq %rcx
    jmp .Lop_call_copy_args
.Lop_call_copy_done:

    movq %r15, %r12                     // pop callee+args off VALSTACK in one shot

    popq %rdx                           // saved ENV (return_env)
    popq %rcx                           // saved IP
    addq $ASNIX_IROP_SIZE, %rcx         // return_ip = saved IP past this CALL record

    movq %rcx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdx, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14) // save the caller's constant pool too (see ir.h)
    movq $0, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14) // ordinary call: no thunk to memoize on return
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq %rax, %rdi                     // ENV = new frame
    movq ASNIX_CLOSURE_OFF_POOL(%rbp), %r13 // POOL = the callee's own pool, not the caller's
    movq ASNIX_CLOSURE_OFF_BODY(%rbp), %rsi // IP = closure body
    movq (%rsi), %rax
    jmp *%rax

.Lop_call_type_error:
    leaq .Lmsg_call_type(%rip), %rdi
    movq $ASNIX_MSG_CALL_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lop_call_arity_error:
    leaq .Lmsg_call_arity(%rip), %rdi
    movq $ASNIX_MSG_CALL_ARITY_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_call, . - asnix_op_call

    .globl asnix_op_ret
    .type asnix_op_ret, @function
asnix_op_ret:
    subq $ASNIX_RETFRAME_SIZE, %r14
    movq ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14), %rcx
    testq %rcx, %rcx
    jz .Lret_no_memo
    // This frame was pushed to force a thunk (OP_VAR_REF/OP_SELECT), not
    // an ordinary call -- the body's last expression already left its
    // result on top of VALSTACK (same place an ordinary call leaves its
    // result); memoize it into the thunk cell before resuming.
    movq -8(%r12), %rdx
    movq %rdx, ASNIX_THUNK_OFF_ENV(%rcx)
    movq $ASNIX_THUNK_STATE_FORCED, ASNIX_THUNK_OFF_STATE(%rcx)
.Lret_no_memo:
    movq ASNIX_RETFRAME_OFF_IP(%r14), %rsi
    movq ASNIX_RETFRAME_OFF_ENV(%r14), %rdi
    movq ASNIX_RETFRAME_OFF_POOL(%r14), %r13
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ret, . - asnix_op_ret
