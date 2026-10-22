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
 * binary-searched by OP_SELECT. Each Attr is {uint32_t sym; Value value;}
 * PACKED to 12 bytes, no padding (M103).
 *
 * Before M103 this was {uint32_t sym; uint32_t pad; Value value;} = 16
 * bytes, with `pad` a permanent, pure-alignment 4-byte tax on every
 * attrset entry ever allocated -- in a never-free arena that's a
 * permanent cost for the process's entire lifetime, not a transient one.
 * Measured on `./asnix d.nix` (nixpkgs's ffmpeg.outPath, M103's
 * instrumentation pass): 12,698,073 total Attr entries ever allocated
 * across 468,792 entries-arrays over the run -- 4 bytes/entry is ~48.4
 * MiB reclaimed on a ~742 MiB peak-RSS run (~6.5%).
 *
 * sym is now read/written as a plain 4-byte field (`movl` in asm,
 * `uint32_t *` in C) -- NOT as an 8-byte field the way the old pad trick
 * let it be (loading/storing `sym+pad` together as one 64-bit word was
 * only safe because `pad` was always zero AND immediately followed by
 * nothing else that 8-byte write/read was allowed to clobber; with no pad,
 * entries[i].sym at offset 0 is immediately followed by entries[i].value's
 * own bytes at offset 4, so any 8-byte (`movq`) access to the sym slot
 * would silently read/corrupt the low 4 bytes of this SAME entry's value
 * -- every sym access in this codebase MUST use the 4-byte-wide form).
 * `value` is unaffected: it's still accessed as a full 8-byte `Value`
 * (`movq`/`Value *`), just at relative offset 4 instead of 8 now.
 *
 * Entry stride is 12, not a power of 2, so every "index * ASNIX_ATTR_SIZE"
 * computation in hand-written asm uses `imulq $12` (one instruction, same
 * as the old `shlq $4` it replaces) rather than a shift. C call sites are
 * unaffected by the non-power-of-2 stride -- `i * ASNIX_ATTR_SIZE` is
 * ordinary C multiplication and the compiler picks whatever instruction
 * sequence it likes.
 *
 * Alignment: `value`'s offset within the overall arena-allocated Attrs
 * object alternates between 8-aligned and 4-aligned (not 8-aligned) as i
 * increases, since 12 isn't a multiple of 8. This is architecturally safe
 * on x86-64: plain `movq`/`Value *` loads and stores never fault on
 * misalignment (only explicitly-aligned SIMD forms like `movdqa` do, and
 * none of those are used on Attr entries -- checked: no SIMD, no atomics,
 * on this array anywhere in the codebase), at most a small, rare
 * cache-line-split cost. An alternative "SoA" layout (all syms packed
 * first, all values packed after, each run individually 8-aligned) would
 * recover slightly more of the 4-byte tax (down to at most 4 bytes of
 * one-time padding PER ARRAY instead of PER ENTRY) but was rejected: it
 * would make every access site's value-offset depend on a runtime `count`
 * (not a compile-time constant), including the hot asm binary-search
 * loops, for a marginal (~1.8 MiB upper bound) additional saving over the
 * straight 12-byte-packed form -- not worth the added complexity/risk. */
#define ASNIX_ATTRS_OFF_COUNT   0
#define ASNIX_ATTRS_OFF_ENTRIES 8
#define ASNIX_ATTR_OFF_SYM      0
#define ASNIX_ATTR_OFF_VALUE    4
#define ASNIX_ATTR_SIZE         12

/* M88: lazy union-layering for `//`. `count` (offset 0 above) is read as a
 * signed int64_t and is never negative for an ordinary flat Attrs -- that
 * sign bit is reused as a sentinel: a NEGATIVE value there means this
 * ASNIX_TAG_ATTRS object is actually a UnionHdr, not a flat entries array:
 *
 *   UnionHdr{ int64_t  neg_leaf_count;  // == -(number of leaves), 1..MAX_LEAVES
 *             Value    flat_cache;      // raw==0 (never a legal Value -- see
 *                                        // value.h: raw 0 decodes as a NULL
 *                                        // ASNIX_TAG_PTR, which no live object
 *                                        // ever is) until materialized, then
 *                                        // the fully flattened+deduped,
 *                                        // sym-sorted flat Attrs Value
 *             Value    leaves[-neg_leaf_count]; }  // lowest-priority (oldest)
 *                                                   // leaf first; each leaf is
 *                                                   // itself an ordinary FLAT
 *                                                   // Attrs Value, never
 *                                                   // another UnionHdr --
 *                                                   // asnix_attrs_update
 *                                                   // (attrs_update.c) always
 *                                                   // resolves a union RHS
 *                                                   // flat before using it as
 *                                                   // one leaf, so unions
 *                                                   // never nest.
 *
 * `asnix_bshared_attrs_resolve` (builtins_shared.c) is the ONE place that
 * knows how to turn a UnionHdr into its flat_cache (materializing +
 * memoizing in place on first need, same "mutate an arena object once,
 * in place" precedent op_select.S's thunk-forcing already established for
 * ASNIX_TAG_THUNK) -- every raw-layout reader of an ATTRS object (every
 * op_*.S binary search, builtins_shared.c's own count/sym/value/find,
 * derivation.c, main.c's printer, builtins_attrs.c, builtins_m54.c) MUST
 * call it first and use ITS result, never poke `count`/`entries` directly
 * on a Value that might be a UnionHdr -- see docs/architecture.md's M88
 * section for the full call-site enumeration this was checked against. */
#define ASNIX_ATTRS_UNION_OFF_NLEAVES   0  /* same slot as OFF_COUNT; stores -(leaf count) */
#define ASNIX_ATTRS_UNION_OFF_FLATCACHE 8
#define ASNIX_ATTRS_UNION_OFF_LEAVES    16
#define ASNIX_ATTRS_UNION_MAX_LEAVES    64

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
