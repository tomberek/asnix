#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "asnix/irpass.h"
#include "asnix/value.h"
#include "compiler.h"
#include "irpass_cfold.h"
#include "irpass_cmpnot.h"
#include "irpass_constprop.h"
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
    "EQ_IF", "NEQ_IF", "LT_IF", "GE_IF", "HAS_ATTR_IF", "LAZY_THUNK_BODY",
};
_Static_assert(sizeof(op_names) / sizeof(op_names[0]) == OP_COUNT,
               "irdump.c's op_names[] is out of sync with ir.h's OP_* enum -- update both");

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

#define IRDUMP_MAX_OPS 200000

static const char *g_dump_src = NULL;

static int byte_pos_to_line(uint32_t pos) {
    int line = 1;
    for (uint32_t i = 0; i < pos && g_dump_src[i]; i++) {
        if (g_dump_src[i] == '\n') line++;
    }
    return line;
}

enum { SNIPPET_CAP = 52 };
static void format_source_snippet(uint32_t pos, char *buf, size_t bufsz) {
    const char *s = g_dump_src;
    uint32_t start = pos;
    while (start > 0 && s[start - 1] != '\n') start--;
    uint32_t end = pos;
    while (s[end] && s[end] != '\n') end++;
    while (start < end && (s[start] == ' ' || s[start] == '\t')) start++;
    uint32_t len = end - start;
    int capped = len > SNIPPET_CAP;
    if (capped) len = SNIPPET_CAP;
    snprintf(buf, bufsz, "%.*s%s", (int)len, s + start, capped ? "..." : "");
}

static void dump_program(IrProgram *prog, int id, SeenVec *seen, Worklist *work) {
    printf("\n=== program #%d (ops=%p pool=%p) ===\n", id, (void *)prog->ops, (void *)prog->pool);
    const uint32_t *prog_lines = asnix_ib_lookup_program_lines(prog);

    int64_t end_idx = -1;
    for (int64_t i = 0; i < IRDUMP_MAX_OPS; i++) {
        int id_of = op_id_of(prog->ops[i].handler);
        if (id_of < 0) break;
        end_idx = i;
        if (id_of == OP_RET || id_of == OP_HALT) break;
    }
    int64_t n_ops = end_idx + 1;
    int *line_no = n_ops ? malloc(sizeof(int) * (size_t)n_ops) : NULL;
    int *line_run = n_ops ? malloc(sizeof(int) * (size_t)n_ops) : NULL;
    for (int64_t i = 0; i < n_ops; i++) {
        line_no[i] = (prog_lines && prog_lines[i] != 0) ? byte_pos_to_line(prog_lines[i]) : 0;
    }
    for (int64_t i = 0; i < n_ops;) {
        int64_t j = i + 1;
        while (j < n_ops && line_no[j] == line_no[i]) j++;
        int run_len = (int)(j - i);
        for (int64_t k = i; k < j; k++) line_run[k] = run_len;
        i = j;
    }

    for (int64_t i = 0; i < IRDUMP_MAX_OPS; i++) {
        IrOp *op = &prog->ops[i];
        int id_of = op_id_of(op->handler);
        if (id_of < 0) {
            printf("  [%4lld] <unrecognized handler %p -- op_names[]/ir.h drift, or past-end garbage>\n", (long long)i,
                   op->handler);
            break;
        }
        char lc_buf[24];
        if (i < n_ops && line_no[i] != 0) {
            snprintf(lc_buf, sizeof lc_buf, "%d:%d", line_no[i], line_run[i]);
        } else if (i < n_ops) {
            snprintf(lc_buf, sizeof lc_buf, "?:%d", line_run[i]);
        } else {
            snprintf(lc_buf, sizeof lc_buf, "?:1");
        }
        char snippet_buf[SNIPPET_CAP + 8] = "";
        if (i < n_ops && line_no[i] != 0 && (i == 0 || line_no[i] != line_no[i - 1])) {
            format_source_snippet(prog_lines[i], snippet_buf, sizeof snippet_buf);
        }
        printf("  [%4lld] %-8s %-19s b=%-5lld", (long long)i, lc_buf, op_names[id_of], (long long)op->b);

        switch (id_of) {
            case OP_PUSH_INT:
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
                if (cid == (int)seen->len - 1) worklist_push(work, child);
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
        {
            size_t fire_counts_here[64] = { 0 };
            size_t npasses = asnix_irpass_num_passes();
            if (npasses > 64) npasses = 64;
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
        if (snippet_buf[0]) printf("   | %s", snippet_buf);
        printf("\n");
        if (id_of == OP_RET || id_of == OP_HALT) break;
    }
    free(line_no);
    free(line_run);
}

static void lex_parse_compile_dump(const char *src, const char *path) {
    g_dump_src = src;
    Ast *ast = asnix_parse(src);

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
    asnix_irpass_register_constprop();
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.nix>\n", argv[0]);
        return 1;
    }
    asnix_arena_init(0);
    ib_enable_line_tracking();

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

    const char *mode = getenv("ASNIX_IR_OPT");
    if (mode && strcmp(mode, "diff") == 0) {
        printf("===== BEFORE (no pass pipeline) =====\n");
        lex_parse_compile_dump(src, argv[1]);
        register_all_passes();
        printf("\n===== AFTER (pass pipeline applied) =====\n");
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

