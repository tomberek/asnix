#include "bytecode.h"

#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * Growable write buffer / bounded read cursor.
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int failed;
} WBuf;

static void wbuf_reserve(WBuf *b, size_t extra) {
    if (b->failed) return;
    if (b->len + extra <= b->cap) return;
    size_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->len + extra) newcap *= 2;
    uint8_t *nb = realloc(b->buf, newcap);
    if (!nb) {
        b->failed = 1;
        return;
    }
    b->buf = nb;
    b->cap = newcap;
}

static void wbuf_bytes(WBuf *b, const void *p, size_t n) {
    wbuf_reserve(b, n);
    if (b->failed) return;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void wbuf_u8(WBuf *b, uint8_t v) {
    wbuf_bytes(b, &v, 1);
}

static void wbuf_i64(WBuf *b, int64_t v) {
    wbuf_bytes(b, &v, 8);
}

static void wbuf_bytes_with_len(WBuf *b, const char *p, size_t n) {
    wbuf_i64(b, (int64_t)n);
    wbuf_bytes(b, p, n);
}

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    int failed;
} RCur;

static void rcur_bytes(RCur *r, void *out, size_t n) {
    if (r->failed || n > r->len - r->pos) {
        r->failed = 1;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
}

static uint8_t rcur_u8(RCur *r) {
    uint8_t v = 0;
    rcur_bytes(r, &v, 1);
    return v;
}

static int64_t rcur_i64(RCur *r) {
    int64_t v = 0;
    rcur_bytes(r, &v, 8);
    return v;
}

/* Arena-owned (never freed -- same lifetime convention every other
 * compiled-in string constant already uses) NUL-free byte copy of the
 * next length-prefixed field; `*out_len` is 0 and the cursor is marked
 * failed on any bounds violation. */
static const char *rcur_bytes_with_len(RCur *r, size_t *out_len) {
    int64_t n = rcur_i64(r);
    if (r->failed || n < 0 || (size_t)n > r->len - r->pos) {
        r->failed = 1;
        *out_len = 0;
        return NULL;
    }
    char *mem = asnix_arena_alloc((size_t)n);
    rcur_bytes(r, mem, (size_t)n);
    *out_len = (size_t)n;
    return mem;
}

/* ---------------------------------------------------------------------
 * Value (de)serialization -- tag + content, never raw pointer bits.
 * Fails (-1) on any heap-pointer kind that should never appear in a
 * compile-time constant pool (List/Closure/Thunk/PrimOp): compile_expr's
 * every ib_push_const call site is Float, a literal String, a literal
 * Path, or (AST_CURPOS only) a small Attrs built from those -- confirmed
 * by reading every push_const call site in compiler.c. This format
 * refuses to guess at anything else rather than half-support it.
 * --------------------------------------------------------------------- */
enum { VTAG_INT = 0, VTAG_IMM = 1, VTAG_STRING = 2, VTAG_PATH = 3, VTAG_FLOAT = 4, VTAG_ATTRS = 5 };

static int write_value(WBuf *b, Value v) {
    int tag = asnix_tag_of(v);
    if (tag == ASNIX_TAG_INT) {
        wbuf_u8(b, VTAG_INT);
        wbuf_i64(b, asnix_int_value(v));
        return 0;
    }
    if (tag == ASNIX_TAG_IMM) {
        wbuf_u8(b, VTAG_IMM);
        wbuf_u8(b, (uint8_t)asnix_imm_subtag(v));
        return 0;
    }
    if (tag == ASNIX_TAG_PTR) {
        if (asnix_value_is_float(v)) {
            double d = asnix_float_value(v);
            wbuf_u8(b, VTAG_FLOAT);
            wbuf_bytes(b, &d, 8);
            return 0;
        }
        int64_t len;
        const char *bytes = asnix_ptr_bytes(v, &len);
        wbuf_u8(b, asnix_value_is_path(v) ? VTAG_PATH : VTAG_STRING);
        wbuf_bytes_with_len(b, bytes, (size_t)len);
        return 0;
    }
    if (tag == ASNIX_TAG_ATTRS) {
        wbuf_u8(b, VTAG_ATTRS);
        int64_t n = asnix_bshared_attrs_count(v);
        wbuf_i64(b, n);
        for (int64_t i = 0; i < n; i++) {
            uint32_t sym = asnix_bshared_attrs_sym(v, i);
            size_t name_len;
            const char *name = asnix_sym_name(sym, &name_len);
            wbuf_bytes_with_len(b, name, name_len);
            if (write_value(b, asnix_bshared_attrs_value(v, i)) != 0) return -1;
        }
        return 0;
    }
    return -1; /* LIST/CLOSURE/THUNK/PRIMOP: never a real pool constant today */
}

/* asnix_bshared_make_attrs unconditionally insertion-sorts its syms/vals
 * arrays in place before building the heap object (builtins_shared.c) --
 * so re-interned, no-longer-sorted-by-new-id entries come out correctly
 * sorted with no extra step needed here. This is the ONE place in this
 * file that's allowed to rely on that -- every other attrset-shaped
 * reconstruction below (OP_MK_ATTRS) does NOT go through
 * asnix_bshared_make_attrs directly (it must also interleave freshly
 * re-ordered value-producing ops into the surrounding bytecode stream,
 * not just build one heap object) and sorts explicitly itself. */
static int read_value(RCur *r, Value *out) {
    uint8_t tag = rcur_u8(r);
    if (r->failed) return -1;
    switch (tag) {
        case VTAG_INT:
            *out = asnix_make_int(rcur_i64(r));
            return r->failed ? -1 : 0;
        case VTAG_IMM: {
            uint8_t sub = rcur_u8(r);
            if (r->failed || sub > ASNIX_IMM_BLACKHOLE) return -1;
            *out = asnix_make_imm(sub);
            return 0;
        }
        case VTAG_FLOAT: {
            double d;
            rcur_bytes(r, &d, 8);
            if (r->failed) return -1;
            *out = asnix_make_float_value(d);
            return 0;
        }
        case VTAG_STRING:
        case VTAG_PATH: {
            size_t len;
            const char *bytes = rcur_bytes_with_len(r, &len);
            if (r->failed) return -1;
            *out = (tag == VTAG_PATH) ? asnix_make_path_value(bytes, len) : asnix_make_string_value(bytes, len);
            return 0;
        }
        case VTAG_ATTRS: {
            int64_t n = rcur_i64(r);
            if (r->failed || n < 0 || n > 1000000) return -1;
            uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
            Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
            int rc = 0;
            for (int64_t i = 0; i < n; i++) {
                size_t name_len;
                const char *name = rcur_bytes_with_len(r, &name_len);
                if (r->failed) {
                    rc = -1;
                    break;
                }
                syms[i] = asnix_intern(name, name_len);
                if (read_value(r, &vals[i]) != 0) {
                    rc = -1;
                    break;
                }
            }
            if (rc == 0) *out = asnix_bshared_make_attrs(syms, vals, n);
            free(syms);
            free(vals);
            return rc;
        }
        default:
            return -1;
    }
}

/* ---------------------------------------------------------------------
 * Op-id <-> handler-pointer translation (ir_link's own mapping, reversed
 * here for serialization; re-applied for deserialization via ir_link
 * itself).
 * --------------------------------------------------------------------- */
static int handler_to_op_id(void *handler) {
    for (int i = 0; i < OP_COUNT; i++) {
        if (asnix_handler_table[i] == handler) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Program (de)serialization.
 *
 * Pool-referencing ops (every one enumerated in ir.h's own OP_* comments
 * -- OP_PUSH_CONST, OP_MK_ATTRS, OP_MK_THUNK, OP_LAMBDA, OP_ASSERT_FAIL,
 * OP_CHECK_FORMALS, OP_REC_OVERRIDES_MERGE) carry their pool payload
 * INLINE, immediately after the op itself, instead of in a separate pool
 * block: pool indices are assigned by strict append order
 * (irbuild.c's ib_pool_add), so replaying pool-payload emission in the
 * same op order during deserialization reconstructs an equivalent
 * relative layout with no index bookkeeping needed at all.
 *
 * OP_MK_ATTRS is the one case needing more than "copy the referenced
 * pool slots verbatim, substituting names for ids": asnix's own
 * OP_MK_ATTRS (src/asm/ops/op_mk_attrs.S) assumes its pool-stored symbol
 * list AND the immediately-preceding `count` value-pushing ops are
 * BOTH already in the same sorted-by-symbol-id order -- it performs no
 * sort of its own, just pops VALSTACK in reverse and pairs position-for-
 * position with the pool's symbol list (see that file's own comment).
 * Symbol ids are assigned per-process by first-seen order (symtab.c),
 * so re-interning a cached entry's names in a FRESH process does not
 * generally preserve "sorted by id" -- the ids themselves differ, and
 * there is no general way to resort a flat bytecode stream's symbol list
 * without also reordering the exact ops that produced each matching
 * value. Fixed by writing an attrset literal's `count` (name, value-op)
 * pairs as an explicitly grouped, UNSORTED unit (write_program's
 * OP_MK_ATTRS case) and sorting by the freshly interned ids only once
 * loaded (read_program's OP_MK_ATTRS case) -- the one point where "the
 * new ids" actually exist. Verified this grouping is always exactly
 * `count` single ops, directly adjacent, with nothing else interleaved:
 * compiler.c's AST_ATTRSET binding loop emits exactly one op per binding
 * (either a thunk-free literal's own single push op -- M90's
 * is_thunk_free_literal -- or exactly one ib_mk_thunk call wrapping a
 * self-contained child program), never a multi-op inline sequence.
 *
 * OP_CHECK_FORMALS's two symbol lists are independently sortable (no
 * coupling to any other op's execution order -- op_check_formals.S pops
 * an Attrs and pushes nothing back, it doesn't correlate with a
 * preceding value-push sequence the way OP_MK_ATTRS does) and
 * OP_REC_OVERRIDES_MERGE's symbol list is positional, not sorted (its
 * own ir.h comment: "in ORIGINAL (OP_FRAME_SET slot) order, NOT
 * sorted" -- tied to OP_FRAME_SET slot indices assigned earlier in the
 * same block), so both are handled with a simpler name-for-name
 * substitution that preserves list order exactly, no resort needed.
 * --------------------------------------------------------------------- */

enum {
    PTAG_OP = 0,        /* ordinary op, no pool payload */
    PTAG_OP_CONST = 1,  /* OP_PUSH_CONST: one Value follows */
    PTAG_OP_THUNK = 2,  /* OP_MK_THUNK: one child program follows */
    PTAG_OP_LAMBDA = 3, /* OP_LAMBDA: arity (a) already written as the op's `a`; one child
                          * program + one Value (formals) follow */
    PTAG_OP_ASSERT = 4, /* OP_ASSERT_FAIL: one length-prefixed message follows */
    PTAG_OP_CHECKF = 5, /* OP_CHECK_FORMALS: total_count + two independently-sortable name lists */
    PTAG_OP_RECOVR = 6, /* OP_REC_OVERRIDES_MERGE: n positional names + one plain int */
    PTAG_OP_ATTRS = 7,  /* OP_MK_ATTRS: count UNSORTED (name, value-op) pairs, grouped */
    PTAG_OP_SYM_A = 8,  /* a plain-int `a` operand carries a bare symbol id (not a pool
                          * index): OP_SELECT, OP_HAS_ATTR -- `a` is written/read as a
                          * length-prefixed name instead of a raw int; `b` passes through
                          * unchanged. Found the hard way: without this, a cached
                          * OP_SELECT's stale symbol id replays verbatim in a fresh
                          * process and can collide with whatever THAT process's symtab
                          * happens to assign the same small integer to -- confirmed via
                          * `builtins.attrNames { y=1; x=2; foo=3; }` failing with
                          * "attribute 'y' missing" on a cache hit, 'y' being whatever
                          * this process coincidentally interned at the stale id
                          * `builtins.attrNames` itself was compiled against. */
    PTAG_OP_SYM_B = 9,  /* same, but the bare symbol id is in `b` -- OP_WITH_TRY,
                          * OP_SELECT_OR (both also have a real jump-delta in `a`, passed
                          * through unchanged). */
};

static int write_program(WBuf *b, IrProgram *prog);
static int write_one_op(WBuf *b, IrOp *op, int64_t *pool);

static size_t program_op_count(IrProgram *prog) {
    size_t n = 0;
    for (; n < 10000000; n++) {
        int op_id = handler_to_op_id(prog->ops[n].handler);
        if (op_id < 0) return (size_t)-1;
        if (op_id == OP_HALT || op_id == OP_RET) return n + 1;
    }
    return (size_t)-1;
}

static int write_program(WBuf *b, IrProgram *prog) {
    size_t n_ops = program_op_count(prog);
    if (n_ops == (size_t)-1) return -1;

    /* Pass 1: mark op indices consumed by a following OP_MK_ATTRS's
     * grouped (name, value) pairs, so the main walk below skips emitting
     * them individually (they're emitted as part of the group instead). */
    uint8_t *consumed = calloc(n_ops, 1);
    if (!consumed) return -1;
    int ok = 1;
    for (size_t i = 0; i < n_ops && ok; i++) {
        int op_id = handler_to_op_id(prog->ops[i].handler);
        if (op_id == OP_MK_ATTRS) {
            int64_t count = prog->ops[i].a;
            if (count < 0 || (size_t)count > i) {
                ok = 0;
                break;
            }
            for (int64_t k = 0; k < count; k++) consumed[i - 1 - (size_t)k] = 1;
        }
    }
    if (!ok) {
        free(consumed);
        return -1;
    }

    wbuf_i64(b, (int64_t)n_ops);
    int rc = 0;
    for (size_t i = 0; i < n_ops && rc == 0;) {
        if (consumed[i]) {
            i++;
            continue;
        }
        int op_id = handler_to_op_id(prog->ops[i].handler);
        if (op_id == OP_MK_ATTRS) {
            int64_t count = prog->ops[i].a;
            int64_t pool_b = prog->ops[i].b;
            size_t start = i - (size_t)count;
            wbuf_u8(b, PTAG_OP_ATTRS);
            wbuf_i64(b, count);
            for (int64_t k = 0; k < count && rc == 0; k++) {
                IrOp *valop = &prog->ops[start + (size_t)k];
                uint32_t sym = (uint32_t)prog->pool[pool_b + k];
                size_t name_len;
                const char *name = asnix_sym_name(sym, &name_len);
                wbuf_bytes_with_len(b, name, name_len);
                /* Each pair's value-op is written as a length-prefixed,
                 * self-contained blob (not inline) so the reader can
                 * skip/reorder pairs by byte length alone, with no need
                 * to parse-then-discard one just to find where the next
                 * starts (which would otherwise leak any nested child
                 * IrProgram that throwaway parse allocates -- found
                 * while writing read_one_tagged_op's PTAG_OP_ATTRS case
                 * below). */
                WBuf sub = { 0 };
                rc = write_one_op(&sub, valop, prog->pool);
                if (rc == 0 && !sub.failed) {
                    wbuf_i64(b, (int64_t)sub.len);
                    wbuf_bytes(b, sub.buf, sub.len);
                } else {
                    rc = -1;
                }
                free(sub.buf);
            }
            i++;
            continue;
        }
        rc = write_one_op(b, &prog->ops[i], prog->pool);
        i++;
    }
    free(consumed);
    return (rc == 0 && !b->failed) ? 0 : -1;
}

static int write_one_op(WBuf *b, IrOp *op, int64_t *pool) {
    int op_id = handler_to_op_id(op->handler);
    if (op_id < 0) return -1;
    switch (op_id) {
        case OP_PUSH_CONST: {
            Value v = { .raw = (uint64_t)pool[op->b] };
            wbuf_u8(b, PTAG_OP_CONST);
            wbuf_i64(b, op->a);
            return write_value(b, v);
        }
        case OP_MK_THUNK: {
            /* M100: pool[op->b] is now a single IrProgram* (ib_mk_thunk
             * pushes one pool slot, not a {ops,pool} pair) -- dereference
             * it directly instead of reading two separate pool slots. */
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            wbuf_u8(b, PTAG_OP_THUNK);
            return write_program(b, child);
        }
        case OP_LAMBDA: {
            /* M100: same single-IrProgram*-slot layout as OP_MK_THUNK
             * above; formals moved from pool[op->b+2] to pool[op->b+1]
             * since the child program no longer occupies two slots. */
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            Value formals = { .raw = (uint64_t)pool[op->b + 1] };
            wbuf_u8(b, PTAG_OP_LAMBDA);
            wbuf_i64(b, op->a); /* arity */
            if (write_program(b, child) != 0) return -1;
            return write_value(b, formals);
        }
        case OP_ASSERT_FAIL: {
            const char *msg = (const char *)(intptr_t)pool[op->b];
            int64_t msg_len = pool[op->b + 1];
            if (msg_len < 0) return -1;
            wbuf_u8(b, PTAG_OP_ASSERT);
            wbuf_bytes_with_len(b, msg, (size_t)msg_len);
            return 0;
        }
        case OP_CHECK_FORMALS: {
            int64_t required_count = op->a;
            int64_t pool_b = op->b;
            int64_t total_count = pool[pool_b];
            if (required_count < 0 || total_count < -1) return -1;
            wbuf_u8(b, PTAG_OP_CHECKF);
            wbuf_i64(b, required_count);
            wbuf_i64(b, total_count);
            for (int64_t i = 0; i < required_count; i++) {
                uint32_t sym = (uint32_t)pool[pool_b + 1 + i];
                size_t len;
                const char *name = asnix_sym_name(sym, &len);
                wbuf_bytes_with_len(b, name, len);
            }
            if (total_count >= 0) {
                for (int64_t i = 0; i < total_count; i++) {
                    uint32_t sym = (uint32_t)pool[pool_b + 1 + required_count + i];
                    size_t len;
                    const char *name = asnix_sym_name(sym, &len);
                    wbuf_bytes_with_len(b, name, len);
                }
            }
            return 0;
        }
        case OP_REC_OVERRIDES_MERGE: {
            int64_t n = op->a;
            int64_t pool_b = op->b;
            if (n < 0) return -1;
            wbuf_u8(b, PTAG_OP_RECOVR);
            wbuf_i64(b, n);
            for (int64_t i = 0; i < n; i++) {
                uint32_t sym = (uint32_t)pool[pool_b + i];
                size_t len;
                const char *name = asnix_sym_name(sym, &len);
                wbuf_bytes_with_len(b, name, len);
            }
            wbuf_i64(b, pool[pool_b + n]); /* overrides_slot: plain int, not a symbol */
            return 0;
        }
        case OP_MK_ATTRS:
            /* Never reached directly -- write_program's own walk always
             * intercepts OP_MK_ATTRS itself (emitting PTAG_OP_ATTRS) and
             * marks its `count` preceding ops `consumed` so they never
             * reach this function individually either. */
            return -1;
        case OP_SELECT:
        case OP_HAS_ATTR: {
            /* `a` is a bare symbol id, not a pool index -- see this
             * file's PTAG_OP_SYM_A comment for why this needs its own
             * case (found via a real failure, not by inspection alone). */
            size_t len;
            const char *name = asnix_sym_name((uint32_t)op->a, &len);
            wbuf_u8(b, PTAG_OP_SYM_A);
            wbuf_i64(b, op_id);
            wbuf_bytes_with_len(b, name, len);
            wbuf_i64(b, op->b);
            return 0;
        }
        case OP_WITH_TRY:
        case OP_SELECT_OR: {
            /* `b` is the bare symbol id here; `a` is a real jump delta,
             * passed through unchanged. See PTAG_OP_SYM_A's comment. */
            size_t len;
            const char *name = asnix_sym_name((uint32_t)op->b, &len);
            wbuf_u8(b, PTAG_OP_SYM_B);
            wbuf_i64(b, op_id);
            wbuf_i64(b, op->a);
            wbuf_bytes_with_len(b, name, len);
            return 0;
        }
        default:
            wbuf_u8(b, PTAG_OP);
            wbuf_i64(b, op_id);
            wbuf_i64(b, op->a);
            wbuf_i64(b, op->b);
            return 0;
    }
}

int asnix_bytecode_serialize(IrProgram *prog, uint8_t **out_buf, size_t *out_len) {
    WBuf b = { 0 };
    wbuf_bytes(&b, "ASNXBC01", 8);
    if (write_program(&b, prog) != 0 || b.failed) {
        free(b.buf);
        return -1;
    }
    *out_buf = b.buf;
    *out_len = b.len;
    return 0;
}

/* ---------------------------------------------------------------------
 * Deserialization: mirrors write_program/write_one_op exactly, emitting
 * a fresh ops[]/pool[] pair (malloc'd, growable the same way
 * irbuild.c's own ib_emit/ib_pool_add grow -- these are handed to
 * ir_link/asnix_eval exactly like ib_finish's own output, so they must
 * use the SAME allocator convention, not the arena: see irbuild.c's
 * IrBuilder for precedent). Returns NULL on ANY anomaly -- no partial
 * programs are ever returned.
 * --------------------------------------------------------------------- */
typedef struct {
    IrOp *ops;
    size_t len, cap;
    int64_t *pool;
    size_t pool_len, pool_cap;
    int failed;
} DBuilder;

static void db_emit(DBuilder *d, int op_id, int64_t a, int64_t b) {
    if (d->failed) return;
    if (d->len == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 16;
        IrOp *n = realloc(d->ops, d->cap * sizeof(IrOp));
        if (!n) {
            d->failed = 1;
            return;
        }
        d->ops = n;
    }
    d->ops[d->len].handler = (void *)(intptr_t)op_id;
    d->ops[d->len].a = a;
    d->ops[d->len].b = b;
    d->len++;
}

static int64_t db_pool_add(DBuilder *d, int64_t v) {
    if (d->failed) return 0;
    if (d->pool_len == d->pool_cap) {
        d->pool_cap = d->pool_cap ? d->pool_cap * 2 : 8;
        int64_t *n = realloc(d->pool, d->pool_cap * sizeof(int64_t));
        if (!n) {
            d->failed = 1;
            return 0;
        }
        d->pool = n;
    }
    d->pool[d->pool_len] = v;
    return (int64_t)(d->pool_len++);
}

static int read_program_into(RCur *r, DBuilder *d);

/* Reads `count` length-prefixed names, re-interns each, sorts the
 * resulting ids ascending, and appends them to `d`'s pool in that sorted
 * order -- shared by OP_CHECK_FORMALS's two independently-sortable lists
 * (see that case's own comment for why this sort is necessary at all:
 * op_check_formals.S binary-searches against them, same invariant
 * OP_MK_ATTRS's own grouping logic protects, just without that op's
 * extra "stay in sync with a preceding value-push sequence" wrinkle). */
static int read_sorted_symbol_list(RCur *r, DBuilder *d, int64_t count) {
    if (count == 0) return 0;
    uint32_t *syms = malloc((size_t)count * sizeof(uint32_t));
    for (int64_t i = 0; i < count; i++) {
        size_t len;
        const char *name = rcur_bytes_with_len(r, &len);
        if (r->failed) {
            free(syms);
            return -1;
        }
        syms[i] = asnix_intern(name, len);
    }
    for (int64_t i = 1; i < count; i++) {
        int64_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t t = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = t;
            j--;
        }
    }
    for (int64_t i = 0; i < count; i++) db_pool_add(d, (int64_t)syms[i]);
    free(syms);
    return 0;
}

static int read_one_tagged_op(RCur *r, DBuilder *d) {
    uint8_t ptag = rcur_u8(r);
    if (r->failed) return -1;
    switch (ptag) {
        case PTAG_OP: {
            int64_t op_id = rcur_i64(r);
            int64_t a = rcur_i64(r);
            int64_t b = rcur_i64(r);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, a, b);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_SYM_A: {
            int64_t op_id = rcur_i64(r);
            size_t len;
            const char *name = rcur_bytes_with_len(r, &len);
            int64_t b = rcur_i64(r);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, (int64_t)asnix_intern(name, len), b);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_SYM_B: {
            int64_t op_id = rcur_i64(r);
            int64_t a = rcur_i64(r);
            size_t len;
            const char *name = rcur_bytes_with_len(r, &len);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, a, (int64_t)asnix_intern(name, len));
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_CONST: {
            int64_t a = rcur_i64(r);
            Value v;
            if (r->failed || read_value(r, &v) != 0) return -1;
            int64_t idx = db_pool_add(d, (int64_t)v.raw);
            db_emit(d, OP_PUSH_CONST, a, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_THUNK: {
            DBuilder child = { 0 };
            if (read_program_into(r, &child) != 0) return -1;
            /* Every nested program needs its OWN ir_link call -- only
             * the outermost one gets linked by asnix_bytecode_deserialize
             * itself. Missing this leaves child.ops[i].handler holding a
             * raw op-id integer (0, 1, 2, ...) instead of a real handler
             * function pointer; op_call.S/force.S then `jmp *that`,
             * jumping to a near-NULL address the instant this thunk is
             * ever forced -- exactly the segfault this fixes (found via
             * gdb: "Program received signal SIGSEGV... 0x000000000000000c",
             * `0xc` being a small op-id, not a code address). */
            ir_link(child.ops, child.len);
            /* M100: Thunk/Closure's runtime consumer expects ONE pool
             * slot holding an IrProgram*, not a {ops,pool} pair -- box
             * the freshly-read child the same way ib_finish does. */
            IrProgram *child_prog = malloc(sizeof(IrProgram));
            child_prog->ops = child.ops;
            child_prog->pool = child.pool;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)child_prog);
            db_emit(d, OP_MK_THUNK, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_LAMBDA: {
            int64_t arity = rcur_i64(r);
            DBuilder child = { 0 };
            if (r->failed || read_program_into(r, &child) != 0) return -1;
            ir_link(child.ops, child.len); /* see PTAG_OP_THUNK's identical comment above */
            Value formals;
            if (read_value(r, &formals) != 0) return -1;
            IrProgram *child_prog = malloc(sizeof(IrProgram));
            child_prog->ops = child.ops;
            child_prog->pool = child.pool;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)child_prog);
            db_pool_add(d, (int64_t)formals.raw);
            db_emit(d, OP_LAMBDA, arity, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_ASSERT: {
            size_t len;
            const char *msg = rcur_bytes_with_len(r, &len);
            if (r->failed) return -1;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)msg);
            db_pool_add(d, (int64_t)len);
            db_emit(d, OP_ASSERT_FAIL, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_CHECKF: {
            int64_t required_count = rcur_i64(r);
            int64_t total_count = rcur_i64(r);
            if (r->failed || required_count < 0 || total_count < -1 || required_count > 1000000 ||
                total_count > 1000000)
                return -1;
            int64_t idx = db_pool_add(d, total_count);
            /* Both lists are "sorted ascending by symbol id" per
             * op_check_formals.S's own header comment (it binary-
             * searches the given Attrs against them) -- re-interning
             * does not preserve that order (ids are assigned by first-
             * seen order per process), so each list is independently
             * re-sorted by its own freshly interned ids before being
             * written into the pool, exactly the same bug class
             * OP_MK_ATTRS's own grouping fix addresses (found the same
             * way: a real "unexpected argument" failure on a cached
             * `{ lib, stdenv, callPackage, ... }`-shaped lambda, not by
             * inspection alone). */
            if (read_sorted_symbol_list(r, d, required_count) != 0) return -1;
            if (total_count >= 0 && read_sorted_symbol_list(r, d, total_count) != 0) return -1;
            db_emit(d, OP_CHECK_FORMALS, required_count, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_RECOVR: {
            int64_t n = rcur_i64(r);
            if (r->failed || n < 0) return -1;
            int64_t idx = -1;
            for (int64_t i = 0; i < n; i++) {
                size_t len;
                const char *name = rcur_bytes_with_len(r, &len);
                if (r->failed) return -1;
                int64_t pidx = db_pool_add(d, (int64_t)asnix_intern(name, len));
                if (i == 0) idx = pidx;
            }
            int64_t overrides_slot = rcur_i64(r);
            if (r->failed) return -1;
            int64_t slot_idx = db_pool_add(d, overrides_slot);
            if (n == 0) idx = slot_idx;
            db_emit(d, OP_REC_OVERRIDES_MERGE, n, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_ATTRS: {
            int64_t count = rcur_i64(r);
            if (r->failed || count < 0 || count > 1000000) return -1;
            uint32_t *new_syms = count > 0 ? malloc((size_t)count * sizeof(uint32_t)) : NULL;
            size_t *order = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            /* Each pair's value-op was written as a length-prefixed,
             * self-contained blob (see write_program's OP_MK_ATTRS
             * case), so finding where each one starts is a pure byte-
             * length skip -- no need to parse-then-discard a pair just
             * to find the next one's offset (which would otherwise leak
             * any nested child IrProgram that throwaway parse
             * allocates). */
            size_t *blob_off = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            size_t *blob_len = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            for (int64_t i = 0; i < count; i++) {
                size_t len;
                const char *name = rcur_bytes_with_len(r, &len);
                if (r->failed) {
                    free(new_syms);
                    free(order);
                    free(blob_off);
                    free(blob_len);
                    return -1;
                }
                new_syms[i] = asnix_intern(name, len);
                int64_t bl = rcur_i64(r);
                if (r->failed || bl < 0 || (size_t)bl > r->len - r->pos) {
                    free(new_syms);
                    free(order);
                    free(blob_off);
                    free(blob_len);
                    return -1;
                }
                blob_off[i] = r->pos;
                blob_len[i] = (size_t)bl;
                r->pos += (size_t)bl;
                order[i] = (size_t)i;
            }
            /* Sort `order` by new_syms ascending -- re-establishing the
             * sorted-by-symbol-id invariant OP_MK_ATTRS's own asm
             * handler trusts without checking (see this file's header
             * comment on why this step exists at all). Small counts
             * (real attrsets are rarely more than a few dozen bindings)
             * make insertion sort fine, same simplicity call
             * asnix_bshared_make_attrs itself already makes. */
            for (size_t i = 1; i < (size_t)count; i++) {
                size_t j = i;
                while (j > 0 && new_syms[order[j - 1]] > new_syms[order[j]]) {
                    size_t t = order[j - 1];
                    order[j - 1] = order[j];
                    order[j] = t;
                    j--;
                }
            }
            int rc = 0;
            int64_t pool_start = -1;
            for (int64_t i = 0; i < count && rc == 0; i++) {
                size_t slot = order[i];
                RCur sub = { .buf = r->buf, .len = blob_off[slot] + blob_len[slot], .pos = blob_off[slot], .failed = 0 };
                if (read_one_tagged_op(&sub, d) != 0 || sub.pos != sub.len) rc = -1;
            }
            if (rc == 0) {
                for (int64_t i = 0; i < count; i++) {
                    int64_t pidx = db_pool_add(d, (int64_t)new_syms[order[i]]);
                    if (i == 0) pool_start = pidx;
                }
                if (count == 0) pool_start = db_pool_add(d, 0);
                db_emit(d, OP_MK_ATTRS, count, pool_start);
            }
            free(new_syms);
            free(order);
            free(blob_off);
            free(blob_len);
            return (rc == 0 && !d->failed) ? 0 : -1;
        }
        default:
            return -1;
    }
}

static int read_program_into(RCur *r, DBuilder *d) {
    int64_t n_ops = rcur_i64(r);
    if (r->failed || n_ops <= 0 || n_ops > 10000000) return -1;
    int64_t emitted_before;
    for (int64_t i = 0; i < n_ops;) {
        emitted_before = (int64_t)d->len;
        if (read_one_tagged_op(r, d) != 0) return -1;
        i += (int64_t)d->len - emitted_before; /* PTAG_OP_ATTRS can net-emit >1 op per group entry */
    }
    return d->failed ? -1 : 0;
}

IrProgram *asnix_bytecode_deserialize(const uint8_t *buf, size_t len) {
    if (len < 8 || memcmp(buf, "ASNXBC01", 8) != 0) return NULL;
    RCur r = { .buf = buf, .len = len, .pos = 8, .failed = 0 };
    DBuilder d = { 0 };
    if (read_program_into(&r, &d) != 0 || r.pos != len) {
        free(d.ops);
        free(d.pool);
        return NULL;
    }
    ir_link(d.ops, d.len);
    IrProgram *prog = malloc(sizeof(IrProgram));
    prog->ops = d.ops;
    prog->pool = d.pool;
    return prog;
}
