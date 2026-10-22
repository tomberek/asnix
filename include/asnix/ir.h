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

/* Closure heap object: {up; prog; arity; formals}. In this minimal model a
 * lambda's frame has exactly `arity` slots (one per parameter, no extra
 * pre-sized local slots) -- any nested `let` inside the body introduces its
 * own separate frame via OP_LET, so a distinct nslots field isn't needed.
 *
 * `prog` (an IrProgram*, M100) replaces what were two separate fields
 * (`body`/`pool`) -- every IrProgram already bundles its own `ops`+`pool`
 * pointers (ir.h's own struct, just below) as a pre-existing 16-byte
 * record, so storing ONE pointer to that record here instead of copying
 * both fields out into the Closure saves 8 bytes per closure (closures/
 * thunks are the dominant allocation kind by count across a real nixpkgs
 * evaluation, per this session's own M87/M96 malloc-tracer measurements)
 * at the cost of one extra pointer-chase (prog->ops / prog->pool, both
 * already in L1 alongside `prog` itself at only 16 bytes) wherever a call
 * site needs the body IP or the pool base.
 *
 * A narrower encoding was considered (a 32-bit index into a table of
 * distinct programs, or a 32-bit offset into a dedicated contiguous
 * region) and measured to buy NOTHING further here specifically:
 * `asnix_arena_alloc` (arena.S) rounds every allocation up to the next
 * multiple of 8 bytes, so a 20-byte Closure (up 8 + a 4-byte field +
 * arity 8) and a 24-byte Closure (up 8 + an 8-byte pointer + arity 8)
 * consume the IDENTICAL 24 bytes from the arena -- the narrower
 * encoding's real cost (a second allocator, a header-encoding scheme,
 * more touched asm call sites across op_call.S/force.S/op_lambda.S/
 * op_mk_thunk.S/op_var_ref*.S/op_select*.S/op_with.S) buys literally
 * nothing once that rounding is accounted for, so the plain pointer is
 * both simpler and loses nothing.
 *
 * `prog` still travels with the closure and gets its `ops` installed as
 * the jump target / its `pool` reinstalled in %r13 on call, restored on
 * return, otherwise a lambda body containing another `OP_LAMBDA` would
 * resolve that inner lambda's body pointer against the *caller's* pool
 * instead of its own (the bug M4's curried lambdas first exposed: M3's
 * single-level fixture never had a lambda body containing another
 * OP_LAMBDA). */
#define ASNIX_CLOSURE_OFF_UP    0
#define ASNIX_CLOSURE_OFF_PROG  8
#define ASNIX_CLOSURE_OFF_ARITY 16
/* `builtins.functionArgs`'s pre-built result Value for this closure (M37)
 * -- a plain attrset {name: hasDefault} for a lambda-pattern (`{a, b ?
 * d, ...}: ...`), or the shared empty-attrset Value for every other
 * lambda shape (matching real Nix: `builtins.functionArgs (a: a)` ==
 * `{}`). Built ONCE at compile time (compiler.c's AST_LAMBDA case, which
 * already has every formal's name/has-default info on hand before
 * freeing it) and stashed in the closure's own pool, same mechanism
 * OP_PUSH_CONST uses for any other constant Value -- not computed at
 * OP_LAMBDA-execution time, since it never varies per-call the way the
 * `up`/`prog` fields (capturing the *current* call's environment) do. */
#define ASNIX_CLOSURE_OFF_FORMALS 24
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
 *   UNFORCED:     env/prog are valid (the deferred computation)
 *   IN_PROGRESS:  currently being forced -- re-entering (e.g. `let x = x;
 *                 in x`) is a cycle, caught and aborted (blackholing).
 *                 While IN_PROGRESS, `env` is repurposed (see M82 below)
 *                 to link this cell onto `asnix_inprogress_head`'s
 *                 intrusive stack -- safe because nothing needs to read
 *                 the ORIGINAL `env` again once its one-time use (the
 *                 jump into the thunk's body) has already happened; see
 *                 each of the five op_*.S/force.S sites that set
 *                 IN_PROGRESS for the exact insertion point.
 *   FORCED:       `env`'s slot has been overwritten with the cached
 *                 Value.raw instead; `prog` is no longer meaningful
 *   FAILED (M82): this thunk's body previously ran to a `throw`/failed
 *                 `assert` that unwound (via `builtins.tryEval`'s
 *                 setjmp/longjmp, see tryeval.c) past this cell's own
 *                 OP_RET -- which, since Nix 2.34, real Nix memoizes
 *                 (conformance fixture:
 *                 eval-okay-tryeval-failed-thunk-reeval.nix): re-forcing
 *                 a FAILED thunk must NOT re-run its body (no second
 *                 `trace` side effect) and must NOT be treated as a
 *                 genuine blackhole (it isn't a cycle, just a stale
 *                 IN_PROGRESS left by the longjmp skipping the normal
 *                 "mark FORCED" epilogue) -- instead it re-raises
 *                 (`asnix_tryeval_raise_or_fatal`, tryeval.c/.h),
 *                 catchable by an enclosing `tryEval` same as the
 *                 original failure was. `env`/`prog` are no
 *                 longer meaningful once FAILED (same as FORCED).
 * Forcing overwrites the cell in place (single-threaded, so no CAS claim
 * protocol is needed the way a parallel evaluator like ~/gnix/iets
 * requires) -- every tagged pointer to this same cell sees the memoized
 * value after the first force, exactly once computed.
 *
 * `prog` (an IrProgram*, M100) replaces what were two separate fields
 * (`body`/`pool`) through M96 -- see ASNIX_CLOSURE_OFF_PROG's identical
 * comment just above for the full rationale (8 bytes saved per thunk,
 * the single most common allocation kind, at the cost of one extra
 * pointer-chase through the already-tiny 16-byte IrProgram record). */
#define ASNIX_THUNK_OFF_STATE 0
#define ASNIX_THUNK_OFF_ENV   8  /* UNFORCED: Env*; IN_PROGRESS: intrusive "prev" link (M82); FORCED: cached Value.raw */
#define ASNIX_THUNK_OFF_PROG  16
#define ASNIX_THUNK_SIZE      24

#define ASNIX_THUNK_STATE_UNFORCED    0
#define ASNIX_THUNK_STATE_IN_PROGRESS 1
#define ASNIX_THUNK_STATE_FORCED      2
#define ASNIX_THUNK_STATE_FAILED      3 /* M82: see the comment block above */

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
    OP_VAR_REF_CELL, /* M92: a = depth, b = slot -- same De Bruijn depth/slot addressing as
                     * OP_VAR_REF, but instead of forcing and pushing the slot's VALUE, this
                     * pushes a Cell wrapping a pointer straight at the slot itself (see
                     * heap.h's ASNIX_CELL_MARKER comment). Emitted ONLY by compiler.c's
                     * `try_cell_ref`, at each of the ten `ib_mk_thunk` call sites, when the
                     * expression about to be thunked is a bare variable reference that
                     * resolves LEXICALLY (never for a `with`-fallback name, which has no
                     * fixed compile-time slot to alias) -- mirrors real Nix's
                     * `ExprVar::maybeThunk` override (eval.cc) of the otherwise-universal
                     * "always allocate a fresh Value" `Expr::maybeThunk` default every OTHER
                     * expression kind still uses (OP_MK_THUNK, unchanged). See
                     * op_var_ref_cell.S. */
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
    OP_MK_THUNK,   /* M6: b = pool index of {body IrProgram*} (M100: a single pointer, not a
                     * {body IrOp*, body pool} pair) -- see OP_LAMBDA's note */
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
    OP_WITH_EXIT,  /* M52: no operands; ENV = ENV->up with ASNIX_ENV_WITH_TAG masked off --
                     * the `with`-frame counterpart of OP_FRAME_EXIT, undoing exactly one
                     * OP_WITH so a non-tail `with ...; body` (e.g. one list/attrset element
                     * among siblings) doesn't leak its frame into whatever depth-relative
                     * OP_VAR_REF the compiler emits next, assuming the *original*
                     * (pre-`with`) scope depths. Plain OP_FRAME_EXIT can't be reused here:
                     * a with-frame's own `up` field has the tag bit OR'd in (see env.h), so
                     * blindly chasing it without masking would leave ENV holding a tagged
                     * (odd) pointer. See compiler.c's AST_WITH case and op_with.S. */
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
                     * in-line. M70: a multi-segment chain (`a.b.c or default`) emits ONE
                     * of these per segment, all patched to the SAME shared landing pad --
                     * see compiler.c's AST_SELECT case -- so the whole chain is protected,
                     * not just the single select any one of these ops corresponds to. */
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
    OP_RESOLVE_SEARCH_PATH, /* `<name>` (M31): pops a String (the search name, pushed as
                     * an ordinary compile-time constant by compiler.c's AST_PATH case),
                     * pushes the resolved, canonicalized Path -- or never returns,
                     * routing through the same catchable-throw mechanism `throw`/failed
                     * `assert` use (real Nix's own `findFile` throws the same exception
                     * class on a miss, confirmed in cppnix's source -- see
                     * asnix_resolve_search_path_value, compiler.c). Deliberately resolved
                     * at *evaluation* time, not compile time like an ordinary path
                     * literal, specifically so this can be caught. */
    OP_HAS_ATTR_DYNAMIC, /* `expr ? ${nameExpr}` (M33): pops the name Value then the target
                     * (reverse push order, same convention as OP_SELECT_DYNAMIC), pushes
                     * TRUE/FALSE -- never fatal, not even on a non-attrset target (matches
                     * plain OP_HAS_ATTR's graceful fallthrough, NOT OP_SELECT_DYNAMIC's
                     * fatal-on-miss). The name must still be a string (fatal otherwise, a
                     * genuine misuse rather than a "not found"). Membership-only, like
                     * OP_HAS_ATTR -- never forces the matched entry's value. See
                     * asnix_bshared_has_attr_dynamic (builtins_shared.c) and
                     * op_has_attr_dynamic.S. Found needed evaluating real nixpkgs:
                     * lib/systems/default.nix has `rustc-target-env ?
                     * ${final.rust.rustcTargetSpec}`. */
    OP_REC_OVERRIDES_MERGE, /* `rec { ... __overrides = {...}; ... }` (M57c), replacing the
                     * plain `OP_FRAME_GET*n + OP_MK_ATTRS` sequence compiler.c's
                     * AST_ATTRSET case normally emits after a rec literal's per-binding
                     * OP_FRAME_SET loop, but ONLY when one of the rec's own binding names
                     * is literally `__overrides`: a = n (the rec's own static binding
                     * count), b = pool index of n+1 int64 entries -- [0..n-1]: each
                     * binding's symbol id, in the SAME original order as its
                     * OP_FRAME_SET slot (slot i's sym is entry i, no translation table
                     * needed); [n]: which slot is `__overrides` itself. Reads the
                     * CURRENT Env (set up by the already-executed OP_FRAME_ALLOC)
                     * directly, with no VALSTACK input -- unlike OP_MK_ATTRS, there's
                     * nothing to pop, the n values all already live in ENV.values[].
                     * Forces slot `meta[n]` and requires TAG_ATTRS (fatal otherwise,
                     * "__overrides must be a set" -- matches real Nix's
                     * `state.forceAttrs(*vOverrides, ...)`, called unconditionally the
                     * moment the rec literal is evaluated, not lazily deferred to first
                     * select). For each (sym, value) pair in that forced override set:
                     * if sym matches one of the n static syms at original index i,
                     * OVERWRITES ENV.values[i] with the override's value (so every
                     * OTHER binding in this same rec that references slot i --
                     * including ones already OP_FRAME_SET before this op runs, since
                     * none of them have been forced yet -- sees the override, not the
                     * original expression); unmatched override keys are appended to the
                     * output as brand-new attrset entries with no Env slot (can't be
                     * self-referenced, matching real Nix). Pushes ONE finished Attrs
                     * Value (the merge of all n static entries, with matches replaced,
                     * plus any unmatched extras) -- same "one Value on top of VALSTACK"
                     * postcondition OP_MK_ATTRS leaves, so the dynamic-`${k} = v;` loop
                     * and OP_FRAME_EXIT that normally follow it don't need to know which
                     * path ran. See asnix_bshared_rec_overrides_merge,
                     * builtins_shared.c. */
    OP_VAR_REF_RAW, /* M92: a = depth, b = slot -- same De Bruijn depth/slot addressing as
                     * OP_VAR_REF, but pushes the slot's raw content verbatim, with NO
                     * forcing at all (not even thunk-forcing) -- the OP_FRAME_GET of depth-
                     * relative reads, needed for the identical reason OP_FRAME_GET itself
                     * was (ir.h's own OP_FRAME_GET comment): OP_VAR_REF's force-on-read is
                     * sometimes wrong. Specifically used by
                     * asnix_bshared_make_curried_native_noforce (builtins_shared.c) to
                     * reassemble a curried multi-arg native builtin's captured arguments
                     * before the final native call -- OP_VAR_REF there would silently force
                     * (and Cell-resolve) each argument on the way back out of the curry
                     * chain's own Env slots, defeating the whole point of a NOFORCE primop
                     * like `elem` wanting its raw, unforced first argument (see that
                     * builtin's own comment, builtins_list2.c, and heap.h's
                     * ASNIX_CELL_MARKER comment for why forcing loses slot-identity). Not
                     * used anywhere a Nix-source-level thunk needs its usual laziness
                     * semantics preserved -- ordinary `let`/list/attrs bindings still go
                     * through OP_MK_THUNK/OP_VAR_REF_CELL/OP_VAR_REF exactly as before. */
    OP_PATH_CONCAT, /* M59: `~`/path-literal interpolation (`./${foo}`, `~/${foo}`, ...):
                     * a = count; pops `count` values (same reverse-push-order convention
                     * as OP_STR_CONCAT), coerces each to a string and concatenates them in
                     * order -- IDENTICAL to OP_STR_CONCAT except for what happens to the
                     * result: real Nix's `ExprConcatStrings::eval` always re-canonicalizes
                     * the whole concatenated string (collapsing `.`/`..`/doubled slashes)
                     * and wraps it back up as a Path, whenever the FIRST operand was
                     * itself a Path (~/nix's eval.cc: "v.mkPath(state.rootPath(CanonPath
                     * (resultStr)), ...)"); it does NOT matter that the first operand's
                     * OWN text was deliberately left un-canonicalized at compile time (see
                     * compiler.c's resolve_path_chunk) -- that's a SEPARATE, narrower skip
                     * (real Nix's own comment: "skip canonization of first path, which
                     * would only be not canonized in the first place if it's coming from a
                     * ./${foo} type path"), only relevant to the no-interpolation-at-all
                     * literal case OP_PATH_CONCAT is never used for. compiler.c's AST_PATH
                     * case only ever emits this op when the first part is guaranteed to
                     * already be a Path constant (see resolve_path_chunk), so
                     * asnix_concat_path (value_util.c) can unconditionally canon+wrap the
                     * final result with no runtime type check. */
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
