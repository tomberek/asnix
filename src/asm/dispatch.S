// M3: the threaded-dispatch evaluator core. Register roles, fixed for the
// whole evaluator (see docs/abi.md):
//   %rsi = IP     (current IrOp*)
//   %rdi = ENV    (current Env*)
//   %r12 = VALSTACK (next free Value slot)
//   %r13 = POOL   (IrProgram->pool base, for OP_LAMBDA's body pointer)
//   %r14 = RETSP  (next free call-frame slot; kept off the native machine
//                  stack so the jmp-chain dispatch loop never grows it)
//   %rbx/%rbp/%r15 = free scratch for individual op handlers (op_let,
//                  op_lambda, op_call use these to hold values across their
//                  own calls to asnix_arena_alloc -- see those files)
//
// %rbx/%rbp/%r15 are ordinary SysV callee-saved registers, so asnix_eval as
// a whole must still preserve the *caller's* values across the entire call
// even though op handlers freely clobber them internally -- otherwise a
// handler stomping %rbp (the C frame pointer at -O0!) corrupts the caller
// the moment asnix_eval returns. The prologue below saves all six
// candidate-scratch registers up front; op_halt.S's epilogue restores them
// in the mirror order.
//
// Every handler ends with the same "NEXT" tail: advance IP by one record,
// load its handler address, and `jmp` straight to it -- no central
// dispatch loop is ever re-entered after the first jump.
#include "asnix/ir.h"

    .equ ASNIX_VALSTACK_BYTES, 32768   // 4096 Values
    .equ ASNIX_RETSTACK_BYTES, 4096    // 128 call frames (ASNIX_RETFRAME_SIZE=32 as of M6's force_target field)

    .text

    .globl asnix_eval
    .type asnix_eval, @function
asnix_eval:                             // Value asnix_eval(IrProgram *prog, Env *root_env)
    .cfi_startproc
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp                       // alignment padding; op_halt.S undoes this first

    movq %rdi, %rbx                     // stash prog (original rbx already safely saved above)
    movq %rsi, %rbp                     // stash root_env (original rbp already safely saved above)

    movq $ASNIX_VALSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r12                     // VALSTACK base

    movq $ASNIX_RETSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r14                     // RETSP base

    movq %rbx, %rcx                     // rcx = prog
    movq %rbp, %rdi                     // ENV = root_env
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi // IP = &prog->ops[0]
    .cfi_endproc                        // from here on this is no longer a conventional call/ret frame

    movq (%rsi), %rax
    jmp *%rax
    .size asnix_eval, . - asnix_eval
