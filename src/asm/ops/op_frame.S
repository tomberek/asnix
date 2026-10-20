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

    .text

    .globl asnix_op_frame_alloc
    .type asnix_op_frame_alloc, @function
asnix_op_frame_alloc:                   // a = nslots
    movq ASNIX_IROP_OFF_A(%rsi), %rbx   // rbx = nslots (callee-saved, survives the call below)
    movq %rsi, %rbp                     // save IP (callee-saved, survives the call below)
    movq %rdi, %r15                     // save current ENV = up-pointer for the new frame

    leaq 8(, %rbx, 8), %rdi             // size = 8 + nslots*8
    call asnix_arena_alloc
    movq %r15, ASNIX_ENV_OFF_UP(%rax)

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
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = slot
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8)

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_set, . - asnix_op_frame_set

    .globl asnix_op_frame_exit
    .type asnix_op_frame_exit, @function
asnix_op_frame_exit:                    // no operands; ENV = ENV->up
    movq ASNIX_ENV_OFF_UP(%rdi), %rdi

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_frame_exit, . - asnix_op_frame_exit
