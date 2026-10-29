#include "bytecode.h"

#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "builtins_shared.h"
#include "compiler.h"
#include "symtab.h"
#include "value_util.h"

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

static uint64_t zigzag_encode(int64_t v) {
    return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}

static int64_t zigzag_decode(uint64_t v) {
    return (int64_t)((v >> 1) ^ (~(v & 1) + 1));
}

static void wbuf_varu64(WBuf *b, uint64_t v) {
    while (v >= 0x80) {
        wbuf_u8(b, (uint8_t)(v & 0x7f) | 0x80);
        v >>= 7;
    }
    wbuf_u8(b, (uint8_t)v);
}

static void wbuf_i64(WBuf *b, int64_t v) {
    wbuf_varu64(b, zigzag_encode(v));
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

static uint64_t rcur_varu64(RCur *r) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t byte = rcur_u8(r);
        if (r->failed) return 0;
        if (shift < 64) v |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) return v;
        shift += 7;
    }
    r->failed = 1;
    return 0;
}

static int64_t rcur_i64(RCur *r) {
    return zigzag_decode(rcur_varu64(r));
}

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
    return -1;
}

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

static int handler_to_op_id(void *handler) {
    for (int i = 0; i < OP_COUNT; i++) {
        if (asnix_handler_table[i] == handler) return i;
    }
    return -1;
}

enum {
    PTAG_OP = 0,
    PTAG_OP_CONST = 1,
    PTAG_OP_THUNK = 2,
    PTAG_OP_LAMBDA = 3,
    PTAG_OP_ASSERT = 4,
    PTAG_OP_CHECKF = 5,
    PTAG_OP_RECOVR = 6,
    PTAG_OP_ATTRS = 7,
    PTAG_OP_SYM_A = 8,
    PTAG_OP_LAZY_MARKER = 11,
    PTAG_OP_SYM_B = 9,
    PTAG_OP_PUSHINT = 10,
};

static int write_program(WBuf *b, IrProgram *prog);
static int write_one_op(WBuf *b, IrOp *op, int64_t *pool);

static int write_lazy_thunk_marker(WBuf *b, void *desc) {
    unsigned int pos;
    if (!asnix_lazy_thunk_descriptor_src_pos(desc, &pos)) return -1;
    const char *file_path = asnix_lazy_thunk_descriptor_file_path(desc);
    const char *base_dir = asnix_lazy_thunk_descriptor_base_dir(desc);
    wbuf_u8(b, PTAG_OP_LAZY_MARKER);
    wbuf_i64(b, (int64_t)pos);
    wbuf_bytes_with_len(b, file_path, strlen(file_path));
    wbuf_bytes_with_len(b, base_dir, strlen(base_dir));

    void *scope = asnix_lazy_thunk_descriptor_scope(desc);
    size_t frame_count = 0;
    for (void *f = scope; f; f = asnix_scope_frame_up(f)) frame_count++;
    void **frames = frame_count ? malloc(frame_count * sizeof(void *)) : NULL;
    size_t fi = 0;
    for (void *f = scope; f; f = asnix_scope_frame_up(f)) frames[fi++] = f;
    wbuf_i64(b, (int64_t)frame_count);
    for (size_t i = frame_count; i-- > 0;) {
        void *f = frames[i];
        int count = asnix_scope_frame_count(f);
        wbuf_i64(b, count);
        wbuf_i64(b, asnix_scope_frame_is_with(f));
        wbuf_i64(b, asnix_scope_frame_has_overrides(f));
        for (int ni = 0; ni < count; ni++) {
            size_t len;
            const char *name = asnix_scope_frame_name(f, ni, &len);
            wbuf_bytes_with_len(b, name, len);
        }
    }
    free(frames);
    return b->failed ? -1 : 0;
}

static size_t program_op_count(IrProgram *prog) {
    size_t n = 0;
    for (; n < 10000000; n++) {
        int op_id = handler_to_op_id(prog->ops[n].handler);
        if (op_id < 0) return (size_t)-1;
        if (op_id == OP_LAZY_THUNK_BODY) return (size_t)-1;
        if (op_id == OP_HALT || op_id == OP_RET) return n + 1;
    }
    return (size_t)-1;
}

static int write_program(WBuf *b, IrProgram *prog) {
    size_t n_ops = program_op_count(prog);
    if (n_ops == (size_t)-1) return -1;

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
        case OP_PUSH_INT: {
            wbuf_u8(b, PTAG_OP_PUSHINT);
            wbuf_i64(b, pool[op->b]);
            return 0;
        }
        case OP_PUSH_CONST: {
            Value v = { .raw = (uint64_t)pool[op->b] };
            wbuf_u8(b, PTAG_OP_CONST);
            wbuf_i64(b, op->a);
            return write_value(b, v);
        }
        case OP_MK_THUNK: {
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            if (handler_to_op_id(child->ops[0].handler) == OP_LAZY_THUNK_BODY) {
                void *desc = (void *)(intptr_t)child->pool[child->ops[0].b];
                return write_lazy_thunk_marker(b, desc);
            }
            wbuf_u8(b, PTAG_OP_THUNK);
            return write_program(b, child);
        }
        case OP_LAMBDA: {
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            Value formals = { .raw = (uint64_t)pool[op->b + 1] };
            wbuf_u8(b, PTAG_OP_LAMBDA);
            wbuf_i64(b, op->a);
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
            wbuf_i64(b, pool[pool_b + n]);
            return 0;
        }
        case OP_MK_ATTRS:
            return -1;
        case OP_SELECT:
        case OP_HAS_ATTR: {
            size_t len;
            const char *name = asnix_sym_name((uint32_t)op->a, &len);
            wbuf_u8(b, PTAG_OP_SYM_A);
            wbuf_i64(b, op_id);
            wbuf_bytes_with_len(b, name, len);
            wbuf_i64(b, op->b);
            return 0;
        }
        case OP_WITH_TRY:
        case OP_SELECT_OR:
        case OP_HAS_ATTR_IF: {
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
    wbuf_bytes(&b, "ASNXBC03", 8);
    if (write_program(&b, prog) != 0 || b.failed) {
        free(b.buf);
        return -1;
    }
    *out_buf = b.buf;
    *out_len = b.len;
    return 0;
}

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
        case PTAG_OP_PUSHINT: {
            int64_t v = rcur_i64(r);
            if (r->failed) return -1;
            int64_t idx = db_pool_add(d, v);
            db_emit(d, OP_PUSH_INT, 0, idx);
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
            ir_link(child.ops, child.len);
            IrProgram *child_prog = malloc(sizeof(IrProgram));
            child_prog->ops = child.ops;
            child_prog->pool = child.pool;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)child_prog);
            db_emit(d, OP_MK_THUNK, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_LAZY_MARKER: {
            int64_t pos = rcur_i64(r);
            size_t fp_len, bd_len;
            const char *fp = rcur_bytes_with_len(r, &fp_len);
            const char *bd = rcur_bytes_with_len(r, &bd_len);
            int64_t frame_count = rcur_i64(r);
            if (r->failed || pos < 0 || pos > 0xFFFFFFFFLL || frame_count < 0 || frame_count > 1000000)
                return -1;
            char *file_path = malloc(fp_len + 1);
            memcpy(file_path, fp, fp_len);
            file_path[fp_len] = '\0';
            char *base_dir = malloc(bd_len + 1);
            memcpy(base_dir, bd, bd_len);
            base_dir[bd_len] = '\0';

            void *scope_chain = NULL;
            for (int64_t fi = 0; fi < frame_count; fi++) {
                int64_t count = rcur_i64(r);
                int64_t is_with = rcur_i64(r);
                int64_t has_overrides = rcur_i64(r);
                if (r->failed || count < 0 || count > 1000000) return -1;
                const char **names = count > 0 ? malloc((size_t)count * sizeof(char *)) : NULL;
                size_t *lens = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
                int ok = 1;
                for (int64_t ni = 0; ni < count; ni++) {
                    size_t nlen;
                    const char *nm = rcur_bytes_with_len(r, &nlen);
                    if (r->failed) {
                        ok = 0;
                        break;
                    }
                    names[ni] = nm;
                    lens[ni] = nlen;
                }
                if (ok) scope_chain = asnix_lazy_scope_push_frame(scope_chain, (int)count, (int)is_with,
                                                                   (int)has_overrides, names, lens);
                free(names);
                free(lens);
                if (!ok) return -1;
            }

            void *desc = asnix_make_cached_lazy_thunk_descriptor((unsigned int)pos, file_path, base_dir,
                                                                  scope_chain);
            DBuilder stub = { 0 };
            int64_t desc_idx = db_pool_add(&stub, (int64_t)(intptr_t)desc);
            db_emit(&stub, OP_LAZY_THUNK_BODY, 0, desc_idx);
            if (stub.failed) return -1;
            ir_link(stub.ops, stub.len);
            IrProgram *stub_prog = malloc(sizeof(IrProgram));
            stub_prog->ops = stub.ops;
            stub_prog->pool = stub.pool;
            asnix_lazy_thunk_descriptor_set_self(desc, stub_prog);

            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)stub_prog);
            db_emit(d, OP_MK_THUNK, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_LAMBDA: {
            int64_t arity = rcur_i64(r);
            DBuilder child = { 0 };
            if (r->failed || read_program_into(r, &child) != 0) return -1;
            ir_link(child.ops, child.len);
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
    if (d->cap == 0) {
        d->ops = malloc((size_t)n_ops * sizeof(IrOp));
        if (!d->ops) return -1;
        d->cap = (size_t)n_ops;
    }
    if (d->pool_cap == 0) {
        d->pool = malloc((size_t)n_ops * sizeof(int64_t));
        if (!d->pool) return -1;
        d->pool_cap = (size_t)n_ops;
    }
    int64_t emitted_before;
    for (int64_t i = 0; i < n_ops;) {
        emitted_before = (int64_t)d->len;
        if (read_one_tagged_op(r, d) != 0) return -1;
        i += (int64_t)d->len - emitted_before;
    }
    return d->failed ? -1 : 0;
}

IrProgram *asnix_bytecode_deserialize(const uint8_t *buf, size_t len) {
    if (len < 8 || memcmp(buf, "ASNXBC03", 8) != 0) return NULL;
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

