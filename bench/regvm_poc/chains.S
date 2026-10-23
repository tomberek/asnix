// Isolated POC microbenchmark for the register-based-VM speed investigation
// (docs/architecture.md M123's cross-evaluator gap; see this directory's
// README.md for the full writeup). NOT linked against libasnix.a -- this
// is a self-contained, hand-written comparison of two implementations of
// the SAME op-handler shapes real asnix uses (op_var_ref.S's
// already-forced/non-Cell fast path, op_arith.S's all-int fast path,
// op_cmp.S's asnix_op_lt fast path), faithfully copied instruction-for-
// instruction from those files, for one representative chain shape:
//
//   (a + b) * (c - d) < (e + f) * (g - h)
//
// -- 8 OP_VAR_REF + 6 arithmetic ops + 1 OP_LT, zero OP_CALL/OP_LET/
// OP_FRAME anywhere in the chain (the slice where step 1's "every
// register is already owned by the hottest handlers" blocker does not
// apply, since those handlers never appear here).
//
// Both entry points use the SAME threaded-dispatch mechanism real asnix
// uses (a flat tape of {handler, a, b} records -- ir.h's exact 24-byte
// layout -- with each handler ending in "advance IP, load next handler,
// jmp *reg"), so dispatch overhead and the OP_VAR_REF env-lookup/tag-check
// cost are IDENTICAL between the two. The only deliberate difference:
//
//   run_valstack_chain: every operand is popped from and every result is
//     pushed to a real memory-resident VALSTACK via %r12 -- exactly
//     op_arith.S/op_cmp.S/op_var_ref.S's own real convention.
//
//   run_regvm_chain: the same 8 variables and their 7 intermediate
//     results live in a fixed set of real registers (%r8-%r11) for the
//     whole chain -- chosen statically per chain position (this requires
//     per-position SPECIALIZED handler code, not one generically-reused
//     handler the way the VALSTACK version's op_add/op_sub/op_mul/op_lt
//     are -- see README.md's "specialization tax" section for why this
//     matters honestly, not just as an implementation detail). OP_VAR_REF
//     still reads its `slot` operand from the tape (not baked in as an
//     immediate) so that cost stays identical to the VALSTACK version too
//     -- the ONLY thing eliminated is the push-to-memory/pop-from-memory
//     round trip itself.
//
// Both versions accumulate a checksum (XOR of each iteration's final
// LT result) across `iters` repetitions so the loop can't be trivially
// elided and so run_harness.c has a real, data-dependent value to print.
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .bss
    .align 16
    .lcomm valstack_buf, 4096

    .section .data
    .align 8
valstack_limit_a:
    .quad 0

    .text

// ============================================================
// Shared "never taken" target: every slow/overflow path in both chains
// jumps here. Never reached by construction (test data is chosen to stay
// comfortably inside ASNIX_INT_MIN/MAX and always INT-tagged) -- ud2
// makes it loud and obvious if that invariant is ever violated by a
// future edit to the test data.
// ============================================================
    .globl regvm_poc_never
    .type regvm_poc_never, @function
regvm_poc_never:
    ud2
    .size regvm_poc_never, . - regvm_poc_never

// ============================================================
// Version A: VALSTACK (memory-resident operand stack), faithful to
// op_var_ref.S / op_arith.S / op_cmp.S's real fast-path instruction
// sequences verbatim.
//
// uint64_t run_valstack_chain(Env *env /*rdi*/, long iters /*rsi*/,
//                              BenchOp *tape /*rdx*/);
// ============================================================
    .globl run_valstack_chain
    .type run_valstack_chain, @function
run_valstack_chain:
    pushq %rbx
    pushq %r12                   // callee-saved per SysV -- this function repurposes it as
                                  // the VALSTACK pointer, so the CALLER's value must survive
    pushq %r13
    xorq %r10, %r10              // checksum accumulator, persists across iterations

    movq %rsi, %rbx               // rbx = iters
    movq %rdx, %r13               // r13 = tape base (constant across the whole run)
    leaq valstack_buf+4096(%rip), %rax
    movq %rax, valstack_limit_a(%rip)   // one-time VALSTACK bound, matches dispatch.S's setup cost

    leaq valstack_buf(%rip), %r12 // VALSTACK pointer
    movq %r13, %rsi               // IP = tape base
    // %rdi already holds ENV (Env*), unchanged for the whole run -- same
    // invariant real asnix's dispatch loop keeps.
    testq %rbx, %rbx
    jz .Lva_done
    movq (%rsi), %rax             // first handler
    jmp *%rax

// ---- OP_VAR_REF, generic/reusable across all 8 slots (reads A=depth,
// B=slot from the tape at runtime -- exactly op_var_ref.S's own shape).
// A is always 0 in this benchmark (every var lives in the SAME frame --
// the "no intervening call/thunk-force boundary" slice by construction),
// so the real multi-hop walk loop body is irrelevant and omitted; the one
// zero-check branch itself is kept for honesty about its (always-not-
// taken, still real) cost.
.Lva_var_ref:
    movq %rdi, %rcx
    movq ASNIX_IROP_OFF_A(%rsi), %rdx
    testq %rdx, %rdx
    jnz regvm_poc_never            // depth != 0 never happens here
    movq ASNIX_IROP_OFF_B(%rsi), %rdx
    movq ASNIX_ENV_OFF_VALUES(%rcx,%rdx,8), %rax   // rax = raw tagged Value from env

    // M6/M92 fast-path checks: not-a-Thunk, not-a-Cell (op_var_ref.S's
    // .Lvar_ref_resolve / .Lvar_ref_check_cell, forced/plain-INT case).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    je regvm_poc_never
    cmpq $ASNIX_TAG_PTR, %rcx
    je regvm_poc_never

    // .Lvar_ref_push: real ASNIX_CHECK_VALSTACK scratch + push.
    leaq 8(%r12), %rcx
    cmpq valstack_limit_a(%rip), %rcx
    ja regvm_poc_never
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- OP_ADD fast path (op_arith.S::asnix_op_add, int/int case verbatim).
.Lva_add:
    subq $8, %r12
    movq (%r12), %rcx             // rhs
    subq $8, %r12
    movq (%r12), %rax             // lhs
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    addq %rcx, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- OP_SUB fast path (op_arith.S::asnix_op_sub verbatim).
.Lva_sub:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    subq %rcx, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- OP_MUL fast path (op_arith.S::asnix_op_mul verbatim).
.Lva_mul:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    imulq %rcx, %rax
    jo regvm_poc_never
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- OP_LT fast path (op_cmp.S::asnix_op_lt verbatim).
.Lva_lt:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovlq %rdx, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- loop control (the tape's 16th, non-real-op entry): pop the
// chain's final result, fold into the checksum, decide whether to run
// another iteration or return.
.Lva_loopctl:
    subq $8, %r12
    movq (%r12), %rcx
    xorq %rcx, %r10
    decq %rbx
    jz .Lva_done
    movq %r13, %rsi                // reset IP to tape base
    leaq valstack_buf(%rip), %r12  // reset VALSTACK pointer
    movq (%rsi), %rax
    jmp *%rax
.Lva_done:
    movq %r10, %rax
    popq %r13
    popq %r12
    popq %rbx
    ret
    .size run_valstack_chain, . - run_valstack_chain

// ============================================================
// Version B: register-resident chain. Same 16-entry tape shape, same
// threaded dispatch, same OP_VAR_REF tag-check sequence, same arithmetic/
// comparison instruction sequences -- the ONLY difference is that each
// handler is specialized to a fixed (src registers -> dst register)
// assignment instead of popping/pushing %r12, so no handler here is
// reused across more than one tape slot (see README.md's honesty note on
// why this specialization is a REAL, unavoidable cost of register
// residency in a non-JIT interpreter, not an implementation shortcut).
//
// Register-slot assignment (mirrors the VALSTACK version's own stack-
// depth trace exactly): %r8/%r9/%r10/%r11 hold up to 4 live "virtual
// stack slots"; %r15 is the cross-iteration checksum accumulator.
//
// uint64_t run_regvm_chain(Env *env /*rdi*/, long iters /*rsi*/,
//                           BenchOp *tape /*rdx*/);
// ============================================================
    .globl run_regvm_chain
    .type run_regvm_chain, @function
run_regvm_chain:
    pushq %rbx
    pushq %r13
    pushq %r15                    // callee-saved per SysV -- repurposed as the cross-
                                   // iteration checksum accumulator; caller's value must survive
    xorq %r15, %r15               // checksum accumulator

    movq %rsi, %rbx                // rbx = iters
    movq %rdx, %r13                // r13 = tape base
    movq %r13, %rsi                 // IP = tape base
    testq %rbx, %rbx
    jz .Lrv_done
    movq (%rsi), %rax
    jmp *%rax

// ---- OP_VAR_REF variants: identical tag-check sequence to .Lva_var_ref,
// differing only in destination register (no VALSTACK push at all).
.Lrv_vr_to_r8:
    movq %rdi, %rcx
    movq ASNIX_IROP_OFF_A(%rsi), %rdx
    testq %rdx, %rdx
    jnz regvm_poc_never
    movq ASNIX_IROP_OFF_B(%rsi), %rdx
    movq ASNIX_ENV_OFF_VALUES(%rcx,%rdx,8), %r8
    movq %r8, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    je regvm_poc_never
    cmpq $ASNIX_TAG_PTR, %rcx
    je regvm_poc_never
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lrv_vr_to_r9:
    movq %rdi, %rcx
    movq ASNIX_IROP_OFF_A(%rsi), %rdx
    testq %rdx, %rdx
    jnz regvm_poc_never
    movq ASNIX_IROP_OFF_B(%rsi), %rdx
    movq ASNIX_ENV_OFF_VALUES(%rcx,%rdx,8), %r9
    movq %r9, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    je regvm_poc_never
    cmpq $ASNIX_TAG_PTR, %rcx
    je regvm_poc_never
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lrv_vr_to_r10:
    movq %rdi, %rcx
    movq ASNIX_IROP_OFF_A(%rsi), %rdx
    testq %rdx, %rdx
    jnz regvm_poc_never
    movq ASNIX_IROP_OFF_B(%rsi), %rdx
    movq ASNIX_ENV_OFF_VALUES(%rcx,%rdx,8), %r10
    movq %r10, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    je regvm_poc_never
    cmpq $ASNIX_TAG_PTR, %rcx
    je regvm_poc_never
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lrv_vr_to_r11:
    movq %rdi, %rcx
    movq ASNIX_IROP_OFF_A(%rsi), %rdx
    testq %rdx, %rdx
    jnz regvm_poc_never
    movq ASNIX_IROP_OFF_B(%rsi), %rdx
    movq ASNIX_ENV_OFF_VALUES(%rcx,%rdx,8), %r11
    movq %r11, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    je regvm_poc_never
    cmpq $ASNIX_TAG_PTR, %rcx
    je regvm_poc_never
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- ADD r8+=r9 -> r8 (frees r9); tag-check/overflow logic identical to
// .Lva_add, operands/result are registers instead of memory.
.Lrv_add_r8_r9:
    movq %r8, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r9, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    addq %rdx, %rax
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r8
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- ADD r9+=r10 -> r9 (frees r10).
.Lrv_add_r9_r10:
    movq %r9, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r10, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    addq %rdx, %rax
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r9
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- SUB r9 = r9 - r10 -> r9 (frees r10).
.Lrv_sub_r9_r10:
    movq %r9, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r10, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    subq %rdx, %rax
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r9
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- SUB r10 = r10 - r11 -> r10 (frees r11).
.Lrv_sub_r10_r11:
    movq %r10, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r11, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    subq %rdx, %rax
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r10
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- MUL r8 *= r9 -> r8 (frees r9).
.Lrv_mul_r8_r9:
    movq %r8, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r9, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    imulq %rdx, %rax
    jo regvm_poc_never
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r8
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- MUL r9 *= r10 -> r9 (frees r10).
.Lrv_mul_r9_r10:
    movq %r9, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r10, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    imulq %rdx, %rax
    jo regvm_poc_never
    movq $ASNIX_INT_MAX, %rcx
    cmpq %rcx, %rax
    jg regvm_poc_never
    movq $ASNIX_INT_MIN, %rcx
    cmpq %rcx, %rax
    jl regvm_poc_never
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, %r9
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- LT r8 <? r9 -> r8 (result replaces r8; chain's final value).
.Lrv_lt_r8_r9:
    movq %r8, %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    movq %r9, %rdx
    movq %rdx, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_INT, %rcx
    jne regvm_poc_never
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rdx
    cmpq %rdx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovlq %rdx, %rax
    movq %rax, %r8
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

// ---- loop control: fold r8 (the chain's final result, already in a
// register -- no memory pop needed) into the checksum.
.Lrv_loopctl:
    xorq %r8, %r15
    decq %rbx
    jz .Lrv_done
    movq %r13, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lrv_done:
    movq %r15, %rax
    popq %r15
    popq %r13
    popq %rbx
    ret
    .size run_regvm_chain, . - run_regvm_chain

// Export the local handler labels' addresses isn't possible for .L-prefixed
// (assembler-local) symbols from C, so a small set of non-local aliases is
// provided below purely so harness.c can build each version's 16-entry
// tape without needing a hand-maintained duplicate of this file's own
// control flow.
    .section .data
    .globl regvm_poc_handler_table
    .type regvm_poc_handler_table, @object
    .align 8
regvm_poc_handler_table:
    // Version A, indices 0..15
    .quad .Lva_var_ref   // 0
    .quad .Lva_add       // 1
    .quad .Lva_sub       // 2
    .quad .Lva_mul       // 3
    .quad .Lva_lt        // 4
    .quad .Lva_loopctl   // 5
    // Version B, indices 6..
    .quad .Lrv_vr_to_r8     // 6
    .quad .Lrv_vr_to_r9     // 7
    .quad .Lrv_vr_to_r10    // 8
    .quad .Lrv_vr_to_r11    // 9
    .quad .Lrv_add_r8_r9    // 10
    .quad .Lrv_add_r9_r10   // 11
    .quad .Lrv_sub_r9_r10   // 12
    .quad .Lrv_sub_r10_r11  // 13
    .quad .Lrv_mul_r8_r9    // 14
    .quad .Lrv_mul_r9_r10   // 15
    .quad .Lrv_lt_r8_r9     // 16
    .quad .Lrv_loopctl      // 17
    .size regvm_poc_handler_table, . - regvm_poc_handler_table
