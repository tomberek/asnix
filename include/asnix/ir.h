#ifndef ASNIX_IR_H
#define ASNIX_IR_H

/*
 * Flat, fixed-width IR: a 24-byte, 8-byte-aligned record per instruction.
 * `handler` starts out holding a small op-id (as emitted by the builder /
 * M4 compiler) and is rewritten in place to a real handler address by
 * ir_link() -- see docs/ir-format.md. Fixed-width records mean "advance to
 * the next instruction" is a pure pointer increment, never a decode.
 */

#define ASNIX_IROP_SIZE        24
#define ASNIX_IROP_OFF_HANDLER 0
#define ASNIX_IROP_OFF_A       8
#define ASNIX_IROP_OFF_B       16

#define ASNIX_IRPROG_OFF_OPS  0
#define ASNIX_IRPROG_OFF_POOL 8

/* Closure heap object: {up; body; arity; pool}. In this minimal model a
 * lambda's frame has exactly `arity` slots (one per parameter, no extra
 * pre-sized local slots) -- any nested `let` inside the body introduces its
 * own separate frame via OP_LET, so a distinct nslots field isn't needed.
 *
 * `pool` is each closure's OWN constant pool (every IrBuilder/IrProgram has
 * one, e.g. for ITS OWN nested lambda literals) -- it must travel with the
 * closure and get reinstalled in %r13 on call / restored on return,
 * otherwise a lambda body containing another `OP_LAMBDA` would resolve that
 * inner lambda's body pointer against the *caller's* pool instead of its
 * own (the bug M4's curried lambdas first exposed: M3's single-level
 * fixture never had a lambda body containing another OP_LAMBDA). */
#define ASNIX_CLOSURE_OFF_UP    0
#define ASNIX_CLOSURE_OFF_BODY  8
#define ASNIX_CLOSURE_OFF_ARITY 16
#define ASNIX_CLOSURE_OFF_POOL  24
#define ASNIX_CLOSURE_SIZE      32

/* Call-frame record on the explicit RETSP stack (kept off the native
 * machine stack, so the jmp-chain dispatch loop never grows it): where to
 * resume (return_ip), which Env to resume in (return_env), and which
 * constant pool was active in the caller (return_pool, for the same reason
 * Closure carries one -- see above). `force_target`: non-zero only when
 * this frame was pushed to force a thunk (OP_VAR_REF/OP_SELECT, M6) rather
 * than to make an ordinary OP_CALL -- OP_RET uses it to know which Thunk
 * cell to memoize the result into before resuming (zero means "ordinary
 * call, no memoization"). */
#define ASNIX_RETFRAME_OFF_IP           0
#define ASNIX_RETFRAME_OFF_ENV          8
#define ASNIX_RETFRAME_OFF_POOL         16
#define ASNIX_RETFRAME_OFF_FORCE_TARGET 24
#define ASNIX_RETFRAME_SIZE             32

/* Thunk heap object (M6): a deferred, memoized computation -- the
 * mechanism behind Nix's laziness. `state` distinguishes:
 *   UNFORCED:     env/body/pool are valid (the deferred computation)
 *   IN_PROGRESS:  currently being forced -- re-entering (e.g. `let x = x;
 *                 in x`) is a cycle, caught and aborted (blackholing)
 *   FORCED:       `env`'s slot has been overwritten with the cached
 *                 Value.raw instead; body/pool are no longer meaningful
 * Forcing overwrites the cell in place (single-threaded, so no CAS claim
 * protocol is needed the way a parallel evaluator like ~/gnix/iets
 * requires) -- every tagged pointer to this same cell sees the memoized
 * value after the first force, exactly once computed. */
#define ASNIX_THUNK_OFF_STATE 0
#define ASNIX_THUNK_OFF_ENV   8  /* UNFORCED: Env*; FORCED: cached Value.raw */
#define ASNIX_THUNK_OFF_BODY  16
#define ASNIX_THUNK_OFF_POOL  24
#define ASNIX_THUNK_SIZE      32

#define ASNIX_THUNK_STATE_UNFORCED    0
#define ASNIX_THUNK_STATE_IN_PROGRESS 1
#define ASNIX_THUNK_STATE_FORCED      2

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>
#include "asnix/env.h"
#include "asnix/value.h"

typedef struct {
    void *handler;
    int64_t a;
    int64_t b;
} IrOp;

enum {
    OP_PUSH_INT,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_NULL,
    OP_VAR_REF,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_IF,
    OP_JMP,
    OP_LET,
    OP_LAMBDA,
    OP_CALL,
    OP_RET,
    OP_HALT,
    OP_PUSH_CONST, /* M5: b = pool index of a fully-formed Value.raw (string literals) */
    OP_MK_LIST,    /* M5: a = count; pops `count` values, builds a List */
    OP_MK_ATTRS,   /* M5: a = count, b = pool index of `count` sorted symbol ids */
    OP_SELECT,     /* M5: a = symbol id; pops an Attrs, pushes the matching value or aborts */
    OP_MK_THUNK,   /* M6: b = pool index of {body IrOp*, body pool} -- see OP_LAMBDA's note */
    OP_FRAME_ALLOC, /* a = nslots; allocates a new Env{up=current ENV, nslots} and installs
                      * it as the current ENV (no popping) -- the recursive-scope building
                      * block: thunks constructed afterward (OP_MK_THUNK) naturally capture
                      * THIS frame, enabling self/sibling reference. Paired with
                      * OP_FRAME_EXIT once all consumers of the frame are done. */
    OP_FRAME_SET,   /* a = slot index; pops one value off VALSTACK and stores it into
                      * slot `a` of the CURRENT Env (set up by OP_FRAME_ALLOC). */
    OP_FRAME_GET,   /* a = slot index (M29); pushes slot `a` of the CURRENT Env verbatim --
                      * raw data movement, deliberately NOT forcing (unlike OP_VAR_REF).
                      * Used only to read back a binding's freshly-OP_FRAME_SET thunk for
                      * `rec {}` attrset construction (compiler.c's AST_ATTRSET case),
                      * where OP_VAR_REF's force-on-read would be wrong: it would eagerly
                      * force every `rec {}` field the instant the literal is constructed,
                      * not lazily on first actual select -- a real bug found evaluating
                      * real nixpkgs, same root-cause class as M25's call-by-value
                      * argument bug (confirmed: `(rec { a = abort "x"; b = 2; }).b`
                      * aborted here instead of returning `2`). */
    OP_FRAME_EXIT,  /* no operands; ENV = ENV->up -- undoes exactly one OP_FRAME_ALLOC,
                      * restoring the enclosing scope so a `let`/`rec {}` used as a
                      * non-tail subexpression doesn't leak its frame into whatever
                      * depth-relative OP_VAR_REF comes after it in the same IR stream. */
    /* "operators" milestone: comparisons/booleans/update/assert. `&&`/
     * `||` are deliberately NOT ops here -- they compile to the existing
     * branch-shaped OP_IF/OP_JMP (see compiler.c's AST_AND/AST_OR cases)
     * so the untaken side is provably never evaluated (short-circuit). */
    OP_EQ,          /* pops rhs,lhs; pushes TRUE/FALSE -- see op_cmp.S for the scope cut
                      * (INT numeric equality, IMM subtag equality, pointer identity for
                      * everything else -- no deep structural equality on lists/attrsets) */
    OP_NEQ,         /* ditto, negated */
    OP_LT,          /* pops rhs,lhs (both must be TAG_INT, else fatal); pushes TRUE/FALSE */
    OP_GT,
    OP_LE,
    OP_GE,
    OP_NOT,         /* in-place: top of stack must be TAG_IMM true/false, else fatal */
    OP_UPDATE,      /* pops rhs,lhs (both must be TAG_ATTRS, else fatal); pushes the
                      * right-biased sorted-array merge (asnix_attrs_update, src/c/attrs_update.c) */
    OP_ASSERT_FAIL, /* b = pool index of {msg ptr, msg len}; unconditionally asnix_fatal()s --
                      * compiler.c's AST_ASSERT only reaches this on the cond-false branch */
    OP_STR_CONCAT, /* strings: a = count; pops `count` values (reverse push order like
                    * OP_MK_LIST), coerces each to a string (same rules as
                    * builtins.toString), concatenates them in order, pushes one
                    * String -- the runtime half of string interpolation. */
    OP_WITH,       /* `with`: pops the with-expr thunk pushed just before this op, binds it
                     * into a new 1-slot Env frame whose `up` field has ASNIX_ENV_WITH_TAG
                     * set (see env.h) so OP_WITH_TRY/the var-ref depth walk can recognize it
                     * as a with-frame rather than an ordinary let/lambda frame. */
    OP_WITH_TRY,   /* a = byte delta to jump to when the symbol isn't in this with-level's
                     * attrs; b = symbol id. Pops the attrs value pushed by a preceding
                     * OP_VAR_REF (which already forced the with-frame's slot) -- fatal if
                     * not an attrset. Binary-searches for `b`; if found, pushes the
                     * (force-if-thunk) matching value and falls through; otherwise jumps by
                     * `a` to try the next enclosing `with` (or OP_WITH_MISS if exhausted). */
    OP_WITH_MISS,  /* All enclosing `with`s were tried and none had the symbol: aborts with
                     * "undefined variable" (mirrors the compile-time error raised when no
                     * `with` is in scope at all -- see compiler.c's AST_VAR case). */
    OP_HAS_ATTR,   /* lambda-patterns: a = symbol id; pops a value, pushes TRUE/FALSE --
                     * FALSE (not a fatal error) if the value isn't even an Attrs, matching
                     * real Nix's `expr ? attr` operator. Never forces the matched entry's
                     * value (membership-only; see op_has_attr.S). */
    OP_CHECK_FORMALS, /* lambda-patterns: a = required-field count, b = pool index -- see
                        * ib_check_formals()/op_check_formals.S for the exact pool layout.
                        * Pops an Attrs (fatal if not one), eagerly validates every required
                        * field is present and (unless the pattern had `...`) every given
                        * field is declared, then pushes nothing back -- mirrors real Nix's
                        * eager (not lazy) argument-pattern validation. Pattern fields'
                        * per-slot binding reuses the existing OP_FRAME_ALLOC/OP_FRAME_SET/
                        * OP_FRAME_EXIT trio above (recursive `let`'s own mechanism --
                        * allocate the frame before constructing the thunks, so defaults get
                        * letrec-style mutual/forward visibility) rather than a second,
                        * duplicate alloc/bind op pair. */
    OP_SELECT_OR,  /* `expr.attr or default`: a = byte delta to jump to (the default's IR)
                     * on a miss, b = symbol id. Pops the select target; non-attrset or
                     * missing key both fall through to the delta jump (not fatal, unlike
                     * plain OP_SELECT -- see op_select_or.S). Found: forces the entry if
                     * it's an unforced thunk (same as OP_SELECT) and falls through
                     * in-line. Scope cut: only protects the single select this op
                     * corresponds to, not every segment of a longer `a.b.c` chain --
                     * see op_select_or.S's header comment. */
    OP_SELECT_DYNAMIC, /* `expr.${nameExpr}`: pops the attribute-name Value then the select
                          * target (reverse push order); interns the name (must be a string,
                          * fatal otherwise) and looks it up at runtime, since (unlike plain
                          * OP_SELECT) the symbol can't be known at compile time -- see
                          * asnix_bshared_select_dynamic (builtins_shared.c) and
                          * op_select_dynamic.S. Fatal on a non-attrset target or a missing
                          * key, matching plain OP_SELECT (not OP_SELECT_OR's graceful
                          * fallthrough) -- `.${expr} or default` is rejected at parse time
                          * instead (see parser.c), so this op never needs a miss path. */
    OP_CONCAT,     /* `++` list concatenation (M23): pops rhs,lhs (both must be TAG_LIST,
                     * else fatal -- see asnix_concat_lists, value_util.c), pushes a fresh
                     * list with lhs's elements followed by rhs's elements. Deferred to C
                     * (not hand-rolled in asm) for the same reason OP_UPDATE is: not a
                     * hot-path op, and a plain memcpy-twice is far easier to get right. */
    OP_DIV,        /* `/` integer division (M23): pops rhs,lhs (both must be TAG_INT, else
                     * fatal; rhs == 0 is also fatal, "division by zero"), pushes the
                     * truncated-toward-zero quotient -- see op_arith.S's asnix_op_div. */
    OP_INSERT_DYNAMIC_ATTR, /* `${nameExpr} = value;` inside a plain `{...}`/`rec {...}`
                     * literal (M28): pops value, name, attrs (reverse push order --
                     * compiler.c's AST_ATTRSET case pushes the already-built static
                     * attrs first, then the name, then the value thunk); interns the
                     * name (must be a string, fatal otherwise) and inserts it into
                     * attrs, fatal on a collision with an existing key (matching real
                     * Nix's own "attribute ... already defined" for a plain literal's
                     * duplicate key) -- see asnix_attrs_insert_dynamic,
                     * attrs_update.c. */
    OP_SELECT_DYNAMIC_OR, /* `expr.${nameExpr} or default` (M28): a = byte delta to jump
                     * to (not found / wrong type) on a miss. Pops the name Value then
                     * the select target (same reverse-push order as OP_SELECT_DYNAMIC).
                     * On a hit: forces the matching entry if it's an unforced thunk and
                     * falls through with it on top of VALSTACK, same shape as
                     * OP_SELECT_OR. See asnix_bshared_select_dynamic_or,
                     * builtins_shared.c. */
    OP_COUNT
};

typedef struct {
    IrOp *ops;
    int64_t *pool;
} IrProgram;

extern void *asnix_handler_table[OP_COUNT];

/* Rewrites every ops[i].handler in place from a small op-id (as emitted by
 * the builder) to the real handler address from asnix_handler_table. Must
 * be called exactly once per ops array before it's passed to asnix_eval. */
void ir_link(IrOp *ops, size_t n);

/* The one public M3 entry point: executes `prog` (already ir_link()ed)
 * starting at prog->ops[0] with `root_env` as the outermost lexical scope,
 * and returns the value left on the operand stack by OP_HALT. */
Value asnix_eval(IrProgram *prog, Env *root_env);

/* M6: forces `v` if it's a thunk (returning the cached value unchanged
 * otherwise), usable from C code with no active asnix_eval invocation --
 * see src/asm/force.S for exactly what this does and doesn't cover. */
Value asnix_force_for_print(Value v);

/* M7+: applies `fn` (a Closure or PrimOp Value -- anything else is a fatal
 * type error) to `argc` arguments, usable from C code with or without an
 * active asnix_eval/asnix_force_for_print/asnix_apply invocation already
 * on the native call stack (brings its own fresh VALSTACK/RETSP, never
 * shares one with an already-active context -- see src/asm/apply.S). This
 * is the general re-entrant "call this Value" mechanism builtins like
 * map/filter/foldl' need; `argc` must exactly match `fn`'s arity (a fatal
 * error otherwise, matching op_call.S's own arity check). */
Value asnix_apply(Value fn, const Value *args, int64_t argc);

#endif /* __ASSEMBLER__ */

#endif /* ASNIX_IR_H */
