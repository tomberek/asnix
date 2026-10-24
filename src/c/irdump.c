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
#include "asnix/irpass.h"
#include "asnix/value.h"
#include "compiler.h"
#include "irpass_cfold.h" /* M118: ASNIX_IR_OPT -- see main() below */
#include "irpass_cmpnot.h"
#include "irpass_dce.h"
#include "irpass_dce_if.h"
#include "irpass_jmpnop.h"
#include "irpass_deadframe.h"
#include "irpass_jmpthread.h"
#include "irpass_notfold.h"
#include "irpass_strconcat.h"
#include "irpass_streq.h"
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
    "EQ_IF", "NEQ_IF", "LT_IF", "GE_IF", "HAS_ATTR_IF",
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
            case OP_PUSH_INT: /* M169: literal lives in the pool now, not inline in `a` */
                printf("   ; pool[%lld] = value:%lld", (long long)op->b, (long long)prog->pool[op->b]);
                break;
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
            case OP_WITH_TRY:
            case OP_HAS_ATTR_IF: {
                size_t nlen;
                const char *n = asnix_sym_name((uint32_t)op->b, &nlen);
                printf("   ; jump_delta=%lld sym=%.*s", (long long)op->a, (int)nlen, n ? n : "?");
                break;
            }
            case OP_IF:
            case OP_JMP:
            case OP_EQ_IF:
            case OP_NEQ_IF:
            case OP_LT_IF:
            case OP_GE_IF: {
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
        /* M118 follow-up: annotate this line with every logged pass
         * firing whose location (asnix_irpass_remove's "kept/folded op
         * ends up at start-1" convention -- irpass.h) is this exact op
         * index -- a direct "where did it fire" marker, not just the
         * aggregate per-pass counts asnix_irpass_print_stats gives at the
         * end. Filtered to THIS program via asnix_irpass_firing_belongs_
         * to_program -- the firing log is one global list shared across
         * every nested program's own ib_finish() call (each program's
         * own indexing independently starts at 0, so raw op-index
         * collisions across DIFFERENT programs are the common case, not
         * an edge case), and without this filter a real file showed
         * markers on lines in programs that never fired at all. Counted
         * per pass (not printed once per raw log entry): a fixpoint
         * re-scan commonly collapses several real, distinct firings onto
         * the same final index (e.g. folding `1+2`, then folding the
         * result against `==3`, both land at index 0 once everything
         * shifts down) -- legitimate, but printing the identical marker
         * 2-3 times in a row is just noise. */
        {
            size_t fire_counts_here[64] = { 0 };
            size_t npasses = asnix_irpass_num_passes();
            if (npasses > 64) npasses = 64; /* defensive -- ASNIX_IRPASS_MAX_PASSES is currently 32 */
            for (size_t fi = 0; fi < asnix_irpass_num_firings(); fi++) {
                if (asnix_irpass_firing_op_index(fi) != (size_t)i) continue;
                if (!asnix_irpass_firing_belongs_to_program(fi, prog)) continue;
                size_t pi = asnix_irpass_firing_pass_index(fi);
                if (pi < npasses) fire_counts_here[pi]++;
            }
            for (size_t pi = 0; pi < npasses; pi++) {
                if (fire_counts_here[pi] == 0) continue;
                if (fire_counts_here[pi] == 1) printf("   <<< %s fired here", asnix_irpass_name(pi));
                else printf("   <<< %s fired here (x%zu)", asnix_irpass_name(pi), fire_counts_here[pi]);
            }
        }
        printf("\n");
        if (id_of == OP_RET || id_of == OP_HALT) break;
    }
}

/* Re-lexes+re-parses+re-compiles `src` from scratch and dumps the result
 * -- called once (ASNIX_IR_OPT unset/1) or twice (ASNIX_IR_OPT=diff,
 * once per side of asnix_irpass_register_cfold/asnix_irpass_clear) from
 * main() below. Re-running the whole pipeline rather than snapshotting
 * ops mid-compile keeps this tool exercising the exact same path
 * ib_finish() takes for a real build, same rationale as this file's own
 * top-of-file comment about decoding the real compiled artifact. */
static void lex_parse_compile_dump(const char *src, const char *path) {
    Ast *ast = asnix_parse(src);

    /* base_dir only affects relative-import resolution inside the
     * compiled program, not this dump's own op-level output -- "." is
     * fine for a standalone decode. */
    IrProgram *root = asnix_compile(ast, ".", path);

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
    free(seen.items);
    free(work.items);
}

static void register_all_passes(void) {
    asnix_irpass_register_cfold();
    asnix_irpass_register_cmpnot();
    asnix_irpass_register_dce();
    asnix_irpass_register_dce_if();
    asnix_irpass_register_jmpnop();
    asnix_irpass_register_jmpthread();
    asnix_irpass_register_strconcat();
    asnix_irpass_register_streq();
    asnix_irpass_register_notfold();
    asnix_irpass_register_deadframe();
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

    /* M118's original flag kept exactly as documented (before.txt/
     * after.txt via two separate invocations), plus a new combined mode
     * for a single-command before/after view -- asnix_irpass_clear()
     * (irpass.h) exists specifically to let one process compile the SAME
     * source both with and without the pipeline for a paired comparison:
     *   ./irdump file.nix                 -- before only (unset, unchanged default)
     *   ASNIX_IR_OPT=1 ./irdump file.nix   -- after only (unchanged)
     *   ASNIX_IR_OPT=diff ./irdump file.nix -- both, one invocation (new) */
    const char *mode = getenv("ASNIX_IR_OPT");
    if (mode && strcmp(mode, "diff") == 0) {
        printf("===== BEFORE (no pass pipeline) =====\n");
        lex_parse_compile_dump(src, argv[1]);
        register_all_passes();
        printf("\n===== AFTER (pass pipeline applied) =====\n");
        /* M118 follow-up: per-op <<< markers above are drawn live from
         * the firing log as each AFTER line prints; this final summary
         * is the same per-pass totals ASNIX_IR_OPT=1 prints below, just
         * also shown here so the combined view is self-contained. */
        lex_parse_compile_dump(src, argv[1]);
        printf("\n-- bytecode pass firing counts --\n");
        asnix_irpass_print_stats(stdout);
        asnix_irpass_clear();
    } else {
        if (mode) {
            register_all_passes();
            printf("(ASNIX_IR_OPT=1 -- showing bytecode AFTER the pass pipeline; <<< markers show where each pass fired)\n");
        }
        lex_parse_compile_dump(src, argv[1]);
        if (mode) {
            printf("\n-- bytecode pass firing counts --\n");
            asnix_irpass_print_stats(stdout);
        }
    }
    free(src);
    return 0;
}
