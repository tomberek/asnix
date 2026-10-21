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
#include "asnix/force_scratch.h" // asnix_eval_scratch_{valstack,retstack} (perf)
#include "asnix/ir.h"

    // Sized generously and bounds-checked (see include/asnix/stackcheck.h)
    // rather than grown dynamically, consistent with this project's
    // never-free bump-arena philosophy -- a single fixed allocation per
    // asnix_eval call, with a loud fatal abort if a pathological program
    // (e.g. thousands of dependent nested `let` bindings, or a single
    // huge list/attrset literal) ever exceeds it. Previously 32768/4096
    // bytes (4096 Values / 128 call frames) with NO bounds check at all --
    // silently corrupting whatever the arena allocated next past that
    // point instead of failing loudly; see docs/architecture.md's "Known
    // bugs" list.
    // M80: right-sized against a real high-water-mark measurement (see
    // docs/architecture.md's M80 scope notes) -- previously 8 MiB/2 MiB,
    // a round-number guess never checked against real usage. Measured
    // peak evaluating `hello`/`hello.src`/`bash`/`gcc`/`stdenv`
    // outPath/drvPath against real nixpkgs (tests/nixpkgs-hash/pkgs.nix):
    // ~28.3 KiB valstack / ~5.2 KiB retstack, consistent across every
    // attribute tried (dominated by the shared nixpkgs/lib/overlay
    // bootstrap common to all of them, not the specific package).
    //
    // VALSTACK: 1 MiB gives a >35x margin over that measured peak, and
    // is independently confirmed to still hold `tests/e2e/
    // m9_large_list.nix`'s 15,000-element literal (~120,000 bytes) with
    // room to spare -- see apply.S's own comment on why that specific
    // test mattered for sizing this project's scratch buffers.
    //
    // RETSTACK: NOT sized off the tiny ~5.2 KiB measured peak -- real
    // Nix (github.com/NixOS/nix, checked directly:
    // src/libexpr/include/nix/expr/eval-settings.hh's `maxCallDepth`
    // Setting) defaults its own `max-call-depth` function-call-nesting
    // guard to 10,000 calls (enforced in `EvalState::addCallDepth`,
    // eval-inline.hh), well below the 60 MiB native C++ stack it
    // separately bumps itself to (`ensureStackSizeAtLeast` in eval.cc)
    // -- i.e. real Nix's own *deliberately friendly* recursion-depth
    // limit is a MUCH better-grounded reference for "how deep can a
    // real recursive Nix program legitimately go" than five packages'
    // worth of measurement, which only reached ~166 RetFrames deep and
    // said nothing about how much deeper a real recursive user function
    // (e.g. called via a naively-recursive `lib` helper over a long
    // list) might need. 1 MiB (32,768 call frames) gives a >3.2x margin
    // over Nix's own 10,000-call default, while still an 2x reduction
    // from the original 2 MiB.
    .equ ASNIX_VALSTACK_BYTES, 1048576 // 131,072 Values, 1 MiB
    .equ ASNIX_RETSTACK_BYTES, 1048576 // 32,768 call frames (ASNIX_RETFRAME_SIZE=32), 1 MiB

    // asnix_eval's own nested-recursion depth (every `import` of an
    // uncached file, eval_file.c, re-enters asnix_eval, and imports can
    // themselves nest) -- same per-depth-cache role/discipline as
    // force.S's asnix_force_depth/apply.S's asnix_apply_depth, see
    // force_scratch.h's header comment on asnix_eval_scratch_valstack
    // for why this one turned out to matter most: a full nixpkgs
    // evaluation imports hundreds of distinct files, each previously
    // minting a fresh 10 MiB (8 MiB + 2 MiB) pair from the never-free
    // bump arena. Decremented in op_halt.S, this function's one exit
    // point.
    .bss
    .align 8
    .globl asnix_eval_depth
asnix_eval_depth:
    .quad 0

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

    // Save the enclosing asnix_eval's (if any -- e.g. the importing file's,
    // when this call is `import`'s nested asnix_eval_file -> asnix_eval,
    // see eval_file.c) VALSTACK/RETSP limits before overwriting them below,
    // restored in op_halt.S's epilogue -- same save/restore force.S already
    // does around ITS nested use, for the identical reason: without this,
    // resuming the outer invocation after this one returns would check its
    // pushes against this (now-defunct) inner buffer's limit instead of its
    // own, either a spurious overflow abort or a missed real one depending
    // on which buffer happens to sit at a lower address.
    movq asnix_valstack_limit(%rip), %rcx
    pushq %rcx
    movq asnix_retsp_limit(%rip), %rcx
    pushq %rcx

    // perf: asnix_eval_scratch_{valstack,retstack} (force_scratch.c)
    // cache one buffer pair PER DEPTH instead of minting a brand-new
    // pair from the never-free bump arena on every single call -- see
    // this file's own asnix_eval_depth comment above. %rbx/%rbp (prog/
    // root_env) survive both calls: ordinary SysV calls, both
    // callee-saved.
    movq asnix_eval_depth(%rip), %rdi
    call asnix_eval_scratch_valstack
    movq %rax, %r12                     // VALSTACK base
    leaq ASNIX_VALSTACK_BYTES(%r12), %rcx
    movq %rcx, asnix_valstack_limit(%rip) // one past the last valid Value slot

    movq asnix_eval_depth(%rip), %rdi
    call asnix_eval_scratch_retstack
    movq %rax, %r14                     // RETSP base
    leaq ASNIX_RETSTACK_BYTES(%r14), %rcx
    movq %rcx, asnix_retsp_limit(%rip)  // one past the last valid RetFrame slot

    incq asnix_eval_depth(%rip)

    movq %rbx, %rcx                     // rcx = prog
    movq %rbp, %rdi                     // ENV = root_env
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi // IP = &prog->ops[0]
    .cfi_endproc                        // from here on this is no longer a conventional call/ret frame

    movq (%rsi), %rax
    jmp *%rax
    .size asnix_eval, . - asnix_eval
