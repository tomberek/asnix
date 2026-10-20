// M6: a C-callable force entry point, used by the top-level printer
// (main.c) to recursively force thunks nested inside lists/attrsets for
// display -- the only thing that needs to force a value from *outside* an
// active asnix_eval dispatch loop so far (ordinary in-language forcing
// happens inline in op_var_ref.S/op_select.S). This is intentionally
// narrow: it's not a general re-entrant builtin-calls-force mechanism
// (that's M7+, once real builtins exist and need it); it just sets up its
// own self-contained mini dispatch context (mirroring asnix_eval's own
// prologue) to run one thunk's body to completion and return the result.
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    // Same silent-corruption-to-loud-abort fix as dispatch.S's main
    // VALSTACK/RETSP (see include/asnix/stackcheck.h and
    // docs/architecture.md's "Known bugs" list): previously 4096/1024
    // bytes with no bounds check; now much larger (still smaller than
    // dispatch.S's, since this context only ever runs one thunk-forcing
    // sub-dispatch at a time) and bounds-checked via the same shared
    // asnix_valstack_limit/asnix_retsp_limit globals the main dispatch
    // loop uses -- save/restore them around this nested use exactly like
    // %r12/%r14 themselves, since this routine can run nested inside an
    // active asnix_eval (a builtin forcing a value, e.g. derivation.c) or
    // even inside another asnix_force_for_print call.
    .equ ASNIX_FORCE_VALSTACK_BYTES, 262144 // 32,768 Values, 256 KiB
    .equ ASNIX_FORCE_RETSTACK_BYTES, 65536  // 2,048 call frames, 64 KiB

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole

    .data
    .align 8
.Lforce_sentinel_ir:
    .quad asnix_force_epilogue
    .quad 0
    .quad 0

    .text

    .globl asnix_force_for_print
    .type asnix_force_for_print, @function
asnix_force_for_print:                  // Value asnix_force_for_print(Value v)
    movq %rdi, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lforce_done                    // not a thunk: return v unchanged

    andq $-8, %rax                      // rax = Thunk*
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    cmpq $ASNIX_THUNK_STATE_FORCED, %rcx
    je .Lforce_cached
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %rcx
    je .Lforce_blackhole

    // Bring our own VALSTACK/RETSP and save every candidate-scratch
    // register, exactly like asnix_eval's prologue (see docs/abi.md) --
    // this call may run when no asnix_eval invocation is live at all.
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %rbx
    pushq %rbp
    pushq %r15
    subq $8, %rsp                       // alignment padding, undone in the epilogue below
    movq %rax, %rbx                     // stash the Thunk* across the alloc calls

    // Save the enclosing dispatch context's VALSTACK/RETSP limits (the
    // outer asnix_eval, or an outer asnix_force_for_print if this call
    // nests inside another one) -- restored in the epilogue below,
    // mirroring how %r12/%r14 themselves are saved/restored. %rax holds
    // the live Thunk* here (see above); %rdx is free.
    movq asnix_valstack_limit(%rip), %rdx
    pushq %rdx
    movq asnix_retsp_limit(%rip), %rdx
    pushq %rdx

    movq $ASNIX_FORCE_VALSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r12
    leaq ASNIX_FORCE_VALSTACK_BYTES(%r12), %rdx
    movq %rdx, asnix_valstack_limit(%rip)

    movq $ASNIX_FORCE_RETSTACK_BYTES, %rdi
    call asnix_arena_alloc
    movq %rax, %r14
    leaq ASNIX_FORCE_RETSTACK_BYTES(%r14), %rdx
    movq %rdx, asnix_retsp_limit(%rip)

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rbx)

    // Sentinel retframe: when the thunk's OP_RET fires, it memoizes into
    // force_target (this thunk) then jumps to the sentinel's handler.
    leaq .Lforce_sentinel_ir(%rip), %rax
    ASNIX_CHECK_RETSP %rcx             // %rcx dead here (last written by the mask check above)
    movq %rax, ASNIX_RETFRAME_OFF_IP(%r14)
    movq $0, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq $0, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rbx, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    movq ASNIX_THUNK_OFF_BODY(%rbx), %rsi
    movq ASNIX_THUNK_OFF_POOL(%rbx), %r13
    movq ASNIX_THUNK_OFF_ENV(%rbx), %rdi
    movq (%rsi), %rax
    jmp *%rax

asnix_force_epilogue:
    movq -8(%r12), %rax                 // result, left on VALSTACK by the thunk body's own OP_RET
    popq %rdx                           // restore the enclosing context's RETSP limit (pushed
    movq %rdx, asnix_retsp_limit(%rip)  // last above, so popped first)
    popq %rdx                           // ditto for the VALSTACK limit
    movq %rdx, asnix_valstack_limit(%rip)
    addq $8, %rsp
    popq %r15
    popq %rbp
    popq %rbx
    popq %r14
    popq %r13
    popq %r12
    ret

.Lforce_cached:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
.Lforce_done:
    ret

.Lforce_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_force_for_print, . - asnix_force_for_print
