#ifndef ASNIX_HEAP_H
#define ASNIX_HEAP_H

/*
 * Heap object layouts for the ASNIX_TAG_PTR (string, path), ASNIX_TAG_LIST,
 * and ASNIX_TAG_ATTRS value kinds (see docs/value-encoding.md). All arena-
 * allocated, never freed, 8-byte aligned.
 */

/* String: {int64_t len; char bytes[len];} -- `len` is always >= 0. */
#define ASNIX_STRING_OFF_LEN   0
#define ASNIX_STRING_OFF_BYTES 8

/* Path (import-paths): a second ASNIX_TAG_PTR kind -- there's no free tag
 * bit to spend on a dedicated ASNIX_TAG_PATH (all 8 slots are already
 * assigned, see value.h), so this is exactly the "header Kind byte"
 * extension value.h's comment on ASNIX_TAG_PTR anticipates. Told apart
 * from a String with a single extra load: a genuine String's leading word
 * is always its own (non-negative) byte length, so a Path reserves the
 * negative sentinel ASNIX_PATH_MARKER there instead, pushing its own
 * length and bytes out one slot:
 *   Path{int64_t marker == ASNIX_PATH_MARKER (-1); int64_t len; char bytes[len];}
 * `bytes` always holds a fully-resolved ABSOLUTE, lexically-canonicalized
 * path (no `.`/`..`/doubled slashes, no trailing slash) -- resolution
 * against the containing file's directory happens once, at compile time
 * (see src/c/compiler.c's AST_PATH case), not at every use. Use
 * asnix_value_is_path()/asnix_ptr_bytes() (src/c/value_util.h) rather than
 * poking these offsets directly -- they dispatch on the marker so callers
 * that don't care which PTR kind they have (printing, hashing, ...) don't
 * have to. */
#define ASNIX_PATH_MARKER     (-1)
#define ASNIX_PATH_OFF_MARKER 0
#define ASNIX_PATH_OFF_LEN    8
#define ASNIX_PATH_OFF_BYTES  16

/* Float (M51): a THIRD `ASNIX_TAG_PTR` kind, alongside String/Path above --
 * there is still no free tag bit for a dedicated `ASNIX_TAG_FLOAT` (all 8
 * slots in value.h are spent, `ASNIX_TAG_PRIMOP` having claimed the last
 * one back in M7), so this follows the exact same "reserved negative
 * marker in the leading word" precedent M15 established for Path:
 *   Float{int64_t marker == ASNIX_FLOAT_MARKER (-2); double value;}
 * Unlike Path (which just shifts its own same-shape payload out one slot
 * after the marker), a double needs the full 64 bits, so this is a
 * genuine second word, not a repurposing -- there is no way to shrink a
 * double into the 61 usable bits an immediate encoding would offer
 * without losing precision, so heap-boxing it (same as every other
 * PTR-tagged kind here) is the only honest option. `asnix_value_is_path`
 * (value_util.c) already dispatches on marker value alone, so `-2` here
 * is automatically distinguished from both a genuine String (whose
 * leading word is always its own non-negative length) and a Path
 * (marker `-1`) with no changes needed there. Use
 * asnix_make_float_value()/asnix_value_is_float()/asnix_float_value()
 * (value_util.h) -- never poke these offsets directly. */
#define ASNIX_FLOAT_MARKER     (-2)
#define ASNIX_FLOAT_OFF_MARKER 0
#define ASNIX_FLOAT_OFF_VALUE  8

/* Cell (M92): a FOURTH `ASNIX_TAG_PTR` kind, alongside String/Path/Float
 * above -- same "reserved negative marker in the leading word" precedent,
 * one word further along:
 *   Cell{int64_t marker == ASNIX_CELL_MARKER (-3); Value *target;}
 * A Cell is NOT a Nix-visible value kind (unlike Path/Float, which are
 * both genuine Nix types) -- it's a purely internal evaluator device
 * mirroring real Nix's `maybeThunk` slot-aliasing optimization
 * (~/nix/gc-unlock/src/libexpr/eval.cc, ExprVar::maybeThunk): instead of
 * allocating a fresh Thunk for a bare-variable sub-expression about to be
 * stored somewhere lazily (a list element, a call argument, a let/attrs
 * binding value -- see compiler.c's `try_cell_ref`), a Cell just wraps a
 * pointer STRAIGHT AT the existing Env slot that variable already lives
 * in, so two independent occurrences of the same bare variable end up
 * *aliasing the same underlying storage* instead of each getting its own,
 * separately-forced copy. This is what lets `==`/the printer recover
 * real Nix's `if (&v1 == &v2) return true` slot-identity shortcut
 * (eval.cc ~line 2919) despite asnix's Values normally being copied
 * in-line everywhere (see docs/value-encoding.md) rather than referenced
 * through a stable address the way real Nix's `Value*` is.
 *
 * `target` points into an Env's `values[]` array (see env.h) -- always
 * arena-allocated, never freed, so the pointer stays valid for the
 * program's entire lifetime; dereferencing it yields another raw
 * (possibly still-unforced-Thunk, or even another Cell, see compiler.c's
 * chained-bare-var case) Value, which every force chokepoint (force.S,
 * op_var_ref.S, op_select.S, op_select_or.S, op_with.S's OP_WITH_TRY) must
 * now treat as "not yet a usable value" exactly like a Thunk, recursively
 * resolving through it -- see each of those files' own Cell-handling
 * comment. Use asnix_make_cell_value()/asnix_value_is_cell()/
 * asnix_cell_target() (src/c/value_util.c) -- never poke these offsets
 * directly. */
#define ASNIX_CELL_MARKER     (-3)
#define ASNIX_CELL_OFF_MARKER 0
#define ASNIX_CELL_OFF_TARGET 8

/* List: {int64_t count; Value elems[count];} */
#define ASNIX_LIST_OFF_COUNT 0
#define ASNIX_LIST_OFF_ELEMS 8

/* Attrs: {int64_t count; Attr entries[count];}, entries sorted by sym --
 * binary-searched by OP_SELECT. Each Attr is {uint32_t sym; uint32_t pad;
 * Value value;} = 16 bytes; sym+pad are read/written together as one
 * 8-byte field (the pad is always zero, since the compiler stores symbol
 * ids as plain non-negative int64_t in the constant pool -- see
 * src/c/compiler.c and op_mk_attrs.S). */
#define ASNIX_ATTRS_OFF_COUNT   0
#define ASNIX_ATTRS_OFF_ENTRIES 8
#define ASNIX_ATTR_OFF_SYM      0
#define ASNIX_ATTR_OFF_VALUE    8
#define ASNIX_ATTR_SIZE         16

/* PrimOp (M7): {void *fn; int64_t arity;} -- a native builtin function.
 * `fn` has signature `Value (*)(const Value *args, int64_t argc)`; OP_CALL
 * invokes it directly with a pointer straight into VALSTACK (the same
 * contiguous arg layout the Closure call path already reads from) rather
 * than copying into a new Env frame -- primops have no lexical body to
 * capture a scope for. */
#define ASNIX_PRIMOP_OFF_FN      0
#define ASNIX_PRIMOP_OFF_ARITY   8
#define ASNIX_PRIMOP_OFF_NOFORCE 16 /* M30: nonzero means op_call.S's primop-call path must
                                      * NOT force this primop's arguments before calling it
                                      * (every existing primop leaves this 0, preserving M23's
                                      * "args are pre-forced" convention unchanged) -- the one
                                      * exception is `tryEval`, which needs its raw, unforced
                                      * argument so it can force it itself inside its own
                                      * setjmp/longjmp catch scope (see builtins_tryeval.c);
                                      * forcing it in op_call.S first, before tryEval's own
                                      * catch is even installed, would make tryEval unable to
                                      * catch anything at all. */
#define ASNIX_PRIMOP_SIZE        24

#endif /* ASNIX_HEAP_H */
