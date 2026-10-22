/* irdump -- a standalone companion tool that lexes+parses+compiles a .nix
 * file through this project's own pipeline and pretty-prints the
 * resulting IrProgram: one line per op (index, mnemonic, resolved
 * operands -- symbol ids resolved to names, jump deltas resolved to
 * target op indices, OP_PUSH_CONST's pool entry resolved to a brief
 * printable form), recursing into every nested child IrProgram reached
 * via OP_LAMBDA/OP_MK_THUNK's pool entry (each such program printed
 * exactly once, keyed by pointer identity, however many op sites share
 * it).
 *
 * Usage: ./irdump <file.nix>
 *
 * NOTE: this intentionally decodes the COMPILED op stream using
 * asnix_handler_table (reversing ir_link()'s handler-pointer rewrite
 * back to an op-id) rather than hooking ib_finish() before linking --
 * so it exercises the exact same artifact asnix_eval() itself runs,
 * not a separate pre-link snapshot. op_names[] below must stay in the
 * same order as ir.h's OP_* enum -- the _Static_assert against OP_COUNT
 * catches a size mismatch (a new op added without updating this file),
 * but NOT a reordering; if ir.h's enum order ever changes, update this
 * array to match. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "symtab.h"
#include "value_util.h"

static const char *op_names[] = {
    "PUSH_INT", "PUSH_TRUE", "PUSH_FALSE", "PUSH_NULL", "VAR_REF", "VAR_REF_CELL", "ADD", "SUB", "MUL", "IF", "JMP",
    "LET", "LAMBDA", "CALL", "RET", "HALT", "PUSH_CONST", "MK_LIST", "MK_ATTRS", "SELECT", "MK_THUNK",
    "FRAME_ALLOC", "FRAME_SET", "FRAME_GET", "FRAME_EXIT", "EQ", "NEQ", "LT", "GT", "LE", "GE", "NOT", "UPDATE",
    "ASSERT_FAIL", "STR_CONCAT", "WITH", "WITH_TRY", "WITH_MISS", "WITH_EXIT", "HAS_ATTR", "CHECK_FORMALS",
    "SELECT_OR", "SELECT_DYNAMIC", "CONCAT", "DIV", "INSERT_DYNAMIC_ATTR", "SELECT_DYNAMIC_OR",
    "RESOLVE_SEARCH_PATH", "HAS_ATTR_DYNAMIC", "REC_OVERRIDES_MERGE", "VAR_REF_RAW", "PATH_CONCAT",
};
_Static_assert(sizeof(op_names) / sizeof(op_names[0]) == OP_COUNT,
               "irdump.c's op_names[] is out of sync with ir.h's OP_* enum -- update both");

/* Reverses ir_link()'s in-place handler-pointer rewrite: finds which
 * asnix_handler_table[] slot this op's (already-linked) handler equals. */
static int op_id_of(void *handler) {
    for (int i = 0; i < OP_COUNT; i++) {
        if (asnix_handler_table[i] == handler) return i;
    }
    return -1;
}

static void print_value_brief(Value v) {
    int tag = asnix_tag_of(v);
    switch (tag) {
        case ASNIX_TAG_INT:
            printf("int:%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_NULL: printf("null"); return;
                case ASNIX_IMM_FALSE: printf("false"); return;
                case ASNIX_IMM_TRUE: printf("true"); return;
                default: printf("imm:%d", asnix_imm_subtag(v)); return;
            }
        case ASNIX_TAG_ATTRS: printf("<attrs>"); return;
        case ASNIX_TAG_LIST: printf("<list>"); return;
        case ASNIX_TAG_CLOSURE: printf("<closure>"); return;
        case ASNIX_TAG_THUNK: printf("<thunk>"); return;
        case ASNIX_TAG_PRIMOP: printf("<primop>"); return;
        case ASNIX_TAG_PTR:
            if (asnix_value_is_float(v)) {
                printf("float:%g", asnix_float_value(v));
                return;
            }
            if (asnix_value_is_path(v)) {
                int64_t len;
                const char *s = asnix_ptr_bytes(v, &len);
                printf("path:\"%.*s\"", (int)len, s);
                return;
            }
            if (asnix_value_is_cell(v)) {
                printf("<cell>");
                return;
            }
            {
                int64_t len;
                const char *s = asnix_ptr_bytes(v, &len);
                printf("str:\"%.*s\"", (int)(len > 60 ? 60 : len), s);
                if (len > 60) printf("...");
            }
            return;
        default:
            printf("tag%d:0x%llx", tag, (unsigned long long)v.raw);
            return;
    }
}

typedef struct {
    IrProgram **items;
    size_t len, cap;
} SeenVec;

static int seen_find_or_add(SeenVec *s, IrProgram *p) {
    for (size_t i = 0; i < s->len; i++) {
        if (s->items[i] == p) return (int)i;
    }
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->items = realloc(s->items, s->cap * sizeof(IrProgram *));
    }
    s->items[s->len] = p;
    return (int)(s->len++);
}

typedef struct {
    IrProgram **items;
    size_t len, cap;
} Worklist;

static void worklist_push(Worklist *w, IrProgram *p) {
    if (w->len == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 16;
        w->items = realloc(w->items, w->cap * sizeof(IrProgram *));
    }
    w->items[w->len++] = p;
}

/* Counts ops until a HALT/RET-free scan would be unsafe (fixed-width IR
 * has no end marker in the struct itself) -- so we instead walk until we
 * hit a terminating op (RET/HALT) OR, defensively, a generous cap, since
 * every real program this project compiles ends in exactly one of those
 * two and we'd rather print a truncation notice than read past the
 * buffer on a malformed/future op shape. */
#define IRDUMP_MAX_OPS 200000

static void dump_program(IrProgram *prog, int id, SeenVec *seen, Worklist *work) {
    printf("\n=== program #%d (ops=%p pool=%p) ===\n", id, (void *)prog->ops, (void *)prog->pool);
    for (int64_t i = 0; i < IRDUMP_MAX_OPS; i++) {
        IrOp *op = &prog->ops[i];
        int id_of = op_id_of(op->handler);
        if (id_of < 0) {
            printf("  [%4lld] <unrecognized handler %p -- op_names[]/ir.h drift, or past-end garbage>\n", (long long)i,
                   op->handler);
            break;
        }
        printf("  [%4lld] %-20s a=%-8lld b=%-8lld", (long long)i, op_names[id_of], (long long)op->a, (long long)op->b);

        switch (id_of) {
            case OP_VAR_REF:
            case OP_VAR_REF_CELL:
            case OP_VAR_REF_RAW:
                printf("   ; depth=%lld slot=%lld", (long long)op->a, (long long)op->b);
                break;
            case OP_SELECT:
            case OP_HAS_ATTR: {
                size_t nlen;
                const char *n = asnix_sym_name((uint32_t)op->a, &nlen);
                printf("   ; sym=%.*s", (int)nlen, n ? n : "?");
                break;
            }
            case OP_SELECT_OR:
            case OP_SELECT_DYNAMIC_OR:
            case OP_WITH_TRY: {
                size_t nlen;
                const char *n = asnix_sym_name((uint32_t)op->b, &nlen);
                printf("   ; jump_delta=%lld sym=%.*s", (long long)op->a, (int)nlen, n ? n : "?");
                break;
            }
            case OP_IF:
            case OP_JMP: {
                /* a = byte delta from THIS op's own slot (see
                 * ib_patch_delta) -- convert to an absolute op index for
                 * readability. */
                long long target = i + op->a / ASNIX_IROP_SIZE;
                printf("   ; -> op #%lld", target);
                break;
            }
            case OP_PUSH_CONST: {
                Value v;
                v.raw = (uint64_t)prog->pool[op->b];
                printf("   ; pool[%lld] = ", (long long)op->b);
                print_value_brief(v);
                break;
            }
            case OP_MK_ATTRS: {
                printf("   ; count=%lld syms=[", (long long)op->a);
                for (int64_t k = 0; k < op->a; k++) {
                    size_t nlen;
                    uint32_t sym = (uint32_t)prog->pool[op->b + k];
                    const char *n = asnix_sym_name(sym, &nlen);
                    printf("%s%.*s", k ? "," : "", (int)nlen, n ? n : "?");
                }
                printf("]");
                break;
            }
            case OP_LAMBDA:
            case OP_MK_THUNK: {
                IrProgram *child = (IrProgram *)(intptr_t)prog->pool[op->b];
                int cid = seen_find_or_add(seen, child);
                if (cid == (int)seen->len - 1) worklist_push(work, child); /* first time seen -- queue it */
                printf("   ; -> program #%d", cid);
                break;
            }
            case OP_ASSERT_FAIL: {
                const char *msg = (const char *)(intptr_t)prog->pool[op->b];
                int64_t len = prog->pool[op->b + 1];
                printf("   ; msg=\"%.*s\"", (int)len, msg);
                break;
            }
            default:
                break;
        }
        printf("\n");
        if (id_of == OP_RET || id_of == OP_HALT) break;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.nix>\n", argv[0]);
        return 1;
    }
    asnix_arena_init(0);

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "irdump: cannot open '%s'\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)size + 1);
    fread(src, 1, (size_t)size, f);
    src[size] = '\0';
    fclose(f);

    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks, src);
    free(toks);

    /* base_dir only affects relative-import resolution inside the
     * compiled program, not this dump's own op-level output -- "." is
     * fine for a standalone decode. */
    IrProgram *root = asnix_compile(ast, ".", argv[1]);

    SeenVec seen = { 0 };
    Worklist work = { 0 };
    int root_id = seen_find_or_add(&seen, root);
    worklist_push(&work, root);
    size_t cursor = 0;
    while (cursor < work.len) {
        IrProgram *p = work.items[cursor++];
        int id = -1;
        for (size_t i = 0; i < seen.len; i++) {
            if (seen.items[i] == p) { id = (int)i; break; }
        }
        dump_program(p, id, &seen, &work);
    }
    printf("\n(%zu distinct program(s), root = #%d)\n", seen.len, root_id);
    return 0;
}
