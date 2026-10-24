// Recursion support (letrec `let`, `rec { ... }`): a scope-introducing
// construct allocates its Env frame and switches ENV to it *before* any of
// its bindings' thunks are built (OP_FRAME_ALLOC), so OP_MK_THUNK naturally
// captures this frame -- giving every binding self/sibling visibility.
// Each binding's freshly-built thunk value is then filed into its slot
// (OP_FRAME_SET). Once the construct is done being consumed (a `let`'s
// body finishes, or a `rec {}` literal's Attrs has been built from the
// frame's slots), OP_FRAME_EXIT pops back to the enclosing ENV so a
// non-tail use of `let`/`rec {}` doesn't leak its frame into whatever
// depth-relative OP_VAR_REF the compiler emitted next assuming the
// *original* (pre-frame) scope depths -- see ir.h and compiler.c's
// AST_LET/AST_ATTRSET cases for the full picture.
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"

    .text

    .globl asnix_op_frame_alloc
    .type asnix_op_frame_alloc, @function
asnix_op_frame_alloc:                   // a = nslots
    movl ASNIX_IROP_OFF_A(%rsi), %ebx   // rbx = nslots (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    leaq ASNIX_ENV_OFF_VALUES(, %rbx, 8), %rdi // size = 4 (up) + nslots*8
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbx, asnix_stats_env_elements(%rip)
    // M118: compress the current ENV (%r15) into the new frame's `up`.
    // Unconditional, no NULL-check: ENV is always a live, non-NULL Env
    // by the time any op handler runs (asnix_eval is never invoked with
    // a NULL root_env -- see eval_file.c).
    movq %r15, %rcx
    subq asnix_env_arena_base(%rip), %rcx
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    movq %rax, %rdi                     // ENV = new frame (slots left uninitialized -- every
                                         // slot is written by an OP_FRAME_SET before anything
                                         // can read it; see compiler.c)
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_alloc, . - asnix_op_frame_alloc

    .globl asnix_op_frame_set
    .type asnix_op_frame_set, @function
asnix_op_frame_set:                     // a = slot index; pops VALSTACK into ENV.values[slot]
    movl ASNIX_IROP_OFF_A(%rsi), %edx   // rdx = slot
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8)

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_set, . - asnix_op_frame_set

    // M29: raw frame read, deliberately NOT forcing -- see this op's own
    // comment in ir.h for why OP_VAR_REF (which always forces) is wrong
    // for the one thing this is used for (reading back a `rec {}`
    // binding's freshly-OP_FRAME_SET thunk to hand to OP_MK_ATTRS).
    .globl asnix_op_frame_get
    .type asnix_op_frame_get, @function
asnix_op_frame_get:                     // a = slot index; pushes ENV.values[slot] verbatim
    movl ASNIX_IROP_OFF_A(%rsi), %edx   // rdx = slot
    movq ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8), %rax

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_get, . - asnix_op_frame_get

    .globl asnix_op_frame_exit
    .type asnix_op_frame_exit, @function
asnix_op_frame_exit:                    // no operands; ENV = ENV->up
    movl ASNIX_ENV_OFF_UP(%rdi), %edi   // M118: zero-extends into %rdi; decompress (no tag
                                         // bit to mask -- an ordinary frame's `up` never has
                                         // ASNIX_ENV_WITH_TAG set)
    addq asnix_env_arena_base(%rip), %rdi

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_exit, . - asnix_op_frame_exit
