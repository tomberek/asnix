#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irpass.h"
#include "asnix/stats.h"
#include "asnix/value.h"
#include "builtins_compare.h"
#include "builtins_shared.h"
#include "compiler.h"
#include "eval_file.h"
#include "flake_eval.h"
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
#include "symtab.h"
#include "value_util.h"

static void print_string_contents(const char *bytes, int64_t len) {
    for (int64_t i = 0; i < len; i++) {
        char c = bytes[i];
        switch (c) {
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '$':
                if (i + 1 < len && bytes[i + 1] == '{') {
                    fputs("\\${", stdout);
                    i++;
                } else {
                    putchar('$');
                }
                break;
            default: putchar(c); break;
        }
    }
}

static void print_string_bytes(const char *bytes, int64_t len) {
    putchar('"');
    print_string_contents(bytes, len);
    putchar('"');
}

static int attr_name_needs_quoting(const char *name, size_t len) {
    static const struct { const char *kw; size_t len; } RESERVED[] = {
        { "let", 3 }, { "in", 2 }, { "if", 2 }, { "then", 4 }, { "else", 4 },
        { "rec", 3 }, { "inherit", 7 }, { "assert", 6 }, { "with", 4 }, { "or", 2 },
    };
    if (len == 0) return 1;
    char c0 = name[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return 1;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '\'' || c == '-';
        if (!ok) return 1;
    }
    for (size_t k = 0; k < sizeof(RESERVED) / sizeof(RESERVED[0]); k++) {
        if (len == RESERVED[k].len && memcmp(name, RESERVED[k].kw, len) == 0) return 1;
    }
    return 0;
}

static void print_attr_name(const char *name, size_t len) {
    if (attr_name_needs_quoting(name, (size_t)len)) {
        putchar('"');
        print_string_contents(name, (int64_t)len);
        putchar('"');
    } else {
        fwrite(name, 1, len, stdout);
    }
}

#define ASNIX_PRINT_MAX_DEPTH 4096
static const void *g_print_active[ASNIX_PRINT_MAX_DEPTH];
static int g_print_active_count = 0;

static int print_active_push(const void *base) {
    for (int i = 0; i < g_print_active_count; i++) {
        if (g_print_active[i] == base) return 0;
    }
    if (g_print_active_count < ASNIX_PRINT_MAX_DEPTH) g_print_active[g_print_active_count++] = base;
    return 1;
}

static void print_active_pop(void) {
    if (g_print_active_count > 0) g_print_active_count--;
}

static const void *g_attrs_seen[ASNIX_PRINT_MAX_DEPTH];
static int g_attrs_seen_count = 0;

static int print_attrs_seen_mark(const void *base) {
    for (int i = 0; i < g_attrs_seen_count; i++) {
        if (g_attrs_seen[i] == base) return 0;
    }
    if (g_attrs_seen_count < ASNIX_PRINT_MAX_DEPTH) g_attrs_seen[g_attrs_seen_count++] = base;
    return 1;
}

typedef struct { const char *name; size_t name_len; Value val; } PrintAttrEntry;

static int cmp_print_attr_entry(const void *a, const void *b) {
    const PrintAttrEntry *ea = a, *eb = b;
    size_t minlen = ea->name_len < eb->name_len ? ea->name_len : eb->name_len;
    int cmp = minlen > 0 ? memcmp(ea->name, eb->name, minlen) : 0;
    if (cmp == 0) cmp = (int)ea->name_len - (int)eb->name_len;
    return cmp;
}

#define ASNIX_PRINT_ATTRS_SORT_INSERTION_MAX 32

static void print_value_concrete(Value v);

static int print_peel_wrappers(Value *v_out, int *pushed_out) {
    Value v = *v_out;
    int pushed = 0;
    for (;;) {
        if (asnix_value_is_cell(v)) {
            Value *target = asnix_cell_target(v);
            if (!print_active_push((const void *)target)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = *target;
            continue;
        }
        if (asnix_tag_of(v) == ASNIX_TAG_THUNK) {
            const void *thunk_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(thunk_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_force_for_print(v);
            continue;
        }
        if (asnix_value_is_lazyapp(v)) {
            const void *lazyapp_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(lazyapp_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_lazyapp(v);
            continue;
        }
        if (asnix_value_is_thunkmap(v)) {
            const void *slot_addr = (const void *)asnix_thunkmap_slot(v);
            if (!print_active_push(slot_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_thunkmap(v);
            continue;
        }
        break;
    }
    *v_out = v;
    *pushed_out = pushed;
    return 1;
}

static void print_value_inner(Value v) {
    int pushed;
    if (!print_peel_wrappers(&v, &pushed)) return;
    print_value_concrete(v);
    for (int i = 0; i < pushed; i++) print_active_pop();
}

static void print_value_concrete(Value v) {
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            printf("%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: printf("true"); return;
                case ASNIX_IMM_FALSE: printf("false"); return;
                case ASNIX_IMM_NULL: printf("null"); return;
                default: printf("<imm %d>", asnix_imm_subtag(v)); return;
            }
        case ASNIX_TAG_PTR: {
            if (asnix_value_is_float(v)) {
                printf("%.6g", asnix_float_value(v));
                return;
            }
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            if (asnix_value_is_path(v)) printf("%.*s", (int)len, bytes);
            else print_string_bytes(bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
            printf("[ ");
            for (int64_t i = 0; i < count; i++) {
                print_value_inner(elems[i]);
                printf(" ");
            }
            printf("]");
            return;
        }
        case ASNIX_TAG_ATTRS: {
            v = asnix_bshared_attrs_resolve(v);
            const char *base = (const char *)(v.raw & ~0x7ULL);
            if (!print_attrs_seen_mark(base)) { printf("«repeated»"); return; }
            int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
            PrintAttrEntry stack_buf[64];
            PrintAttrEntry *entries = count <= 64 ? stack_buf : malloc((uint64_t)count * sizeof(PrintAttrEntry));
            for (int64_t i = 0; i < count; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
                entries[i].name = asnix_sym_name(sym, &entries[i].name_len);
                entries[i].val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            }
            if (count <= ASNIX_PRINT_ATTRS_SORT_INSERTION_MAX) {
                for (int64_t i = 1; i < count; i++) {
                    int64_t j = i;
                    PrintAttrEntry e = entries[i];
                    while (j > 0 && cmp_print_attr_entry(&entries[j - 1], &e) > 0) {
                        entries[j] = entries[j - 1];
                        j--;
                    }
                    entries[j] = e;
                }
            } else {
                qsort(entries, (size_t)count, sizeof(PrintAttrEntry), cmp_print_attr_entry);
            }
            printf("{ ");
            for (int64_t i = 0; i < count; i++) {
                print_attr_name(entries[i].name, entries[i].name_len);
                printf(" = ");
                print_value_inner(entries[i].val);
                printf("; ");
            }
            printf("}");
            if (entries != stack_buf) free(entries);
            return;
        }
        case ASNIX_TAG_PRIMOP:
            printf("<PRIMOP>");
            return;
        case ASNIX_TAG_CLOSURE: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            Value formals = *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
            if (formals.raw == asnix_bshared_native_formals_marker().raw) {
                void *up = *(void **)(base + ASNIX_CLOSURE_OFF_UP);
                printf(up == NULL ? "<PRIMOP>" : "<PRIMOP-APP>");
            } else {
                printf("<LAMBDA>");
            }
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

static void print_value(Value v) {
    g_attrs_seen_count = 0;
    print_value_inner(v);
    printf("\n");
}

typedef struct {
    const char *name;
    Value value;
} CliArg;

static Value auto_call(Value fn, const CliArg *args, size_t nargs);

/* Matches real Nix's `findAlongAttrPath` (`~/nix/gc-unlock/src/libexpr/
 * attr-path.cc`) exactly, confirmed directly against `nix-instantiate
 * --eval -A`: a function is auto-called (its formals filled from `args`,
 * falling back to each formal's own default) BEFORE every single
 * indexing step, including the very first -- `-A ffmpeg.outPath` on a
 * file whose top level is itself a function (nixpkgs' own default.nix)
 * needs the root auto-called before `.ffmpeg` can be selected at all.
 * This happens UNCONDITIONALLY, regardless of whether `args` is empty --
 * unlike the SEPARATE, --arg-gated auto-call `main()` applies to the
 * fully-resolved TERMINAL value afterward (confirmed: `-A f` on `{ f =
 * x: x; }` prints the function itself, uncalled, when no --arg is
 * given -- the path-walk's own auto-call never reaches past the last
 * segment). */
static Value select_attrpath(Value v, const char *attrpath, const CliArg *args, size_t nargs) {
    const char *p = attrpath;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seglen = dot ? (size_t)(dot - p) : strlen(p);
        v = asnix_force_for_print(v);
        v = auto_call(v, args, nargs);
        if (asnix_tag_of(v) != ASNIX_TAG_ATTRS) {
            fprintf(stderr, "asnix: -A: '%.*s' is not a set\n", (int)seglen, p);
            exit(1);
        }
        uint32_t sym = asnix_intern(p, seglen);
        int64_t idx = asnix_bshared_attrs_find(v, sym);
        if (idx < 0) {
            fprintf(stderr, "asnix: -A: attribute '%.*s' missing\n", (int)seglen, p);
            exit(1);
        }
        v = asnix_bshared_attrs_value(v, idx);
        p = dot ? dot + 1 : p + seglen;
    }
    return asnix_force_for_print(v);
}

static Value auto_call(Value fn, const CliArg *args, size_t nargs) {
    if (asnix_tag_of(fn) != ASNIX_TAG_CLOSURE) return fn;
    const char *base = (const char *)(uintptr_t)(fn.raw & ~(uint64_t)0x7);
    Value formals = *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
    int64_t nformals = asnix_bshared_attrs_count(formals);
    if (nformals == 0) return fn;

    uint32_t *syms = malloc((size_t)nformals * sizeof(uint32_t));
    Value *vals = malloc((size_t)nformals * sizeof(Value));
    int64_t total = 0;
    for (int64_t i = 0; i < nformals; i++) {
        uint32_t fsym = asnix_bshared_attrs_sym(formals, i);
        size_t flen;
        const char *fname = asnix_sym_name(fsym, &flen);
        int64_t matched = -1;
        for (size_t j = 0; j < nargs; j++) {
            if (strlen(args[j].name) == flen && memcmp(args[j].name, fname, flen) == 0) {
                matched = (int64_t)j;
                break;
            }
        }
        if (matched >= 0) {
            syms[total] = fsym;
            vals[total] = args[matched].value;
            total++;
        } else if (asnix_imm_subtag(asnix_bshared_attrs_value(formals, i)) != ASNIX_IMM_TRUE) {
            fprintf(stderr, "asnix: you must specify a value for the argument '%.*s'\n", (int)flen, fname);
            exit(1);
        }
    }
    Value args_attrs = asnix_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    return asnix_apply(fn, &args_attrs, 1);
}

int main(int argc, char **argv) {
    asnix_arena_init(0);
    asnix_lazy_thunk_enabled = getenv("ASNIX_LAZY_THUNK") != NULL;
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_cfold();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_cmpnot();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_dce();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_dce_if();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_jmpnop();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_jmpthread();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_strconcat();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_streq();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_notfold();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_deadframe();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_constprop();

    const char *file_path = NULL;
    const char *flake_ref = NULL;
    const char *attrpath = NULL;
    const char *expr_source = NULL;
    int want_json = 0;
    int want_raw = 0;
    CliArg *cli_args = NULL;
    size_t n_cli_args = 0, cap_cli_args = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flake") == 0 && i + 1 < argc) {
            flake_ref = argv[i + 1];
            i += 1;
        } else if ((strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "--expr") == 0) && i + 1 < argc) {
            expr_source = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            want_json = 1;
        } else if (strcmp(argv[i], "--raw") == 0) {
            want_raw = 1;
        } else if (strcmp(argv[i], "--arg") == 0 && i + 2 < argc) {
            if (n_cli_args == cap_cli_args) {
                cap_cli_args = cap_cli_args ? cap_cli_args * 2 : 4;
                cli_args = realloc(cli_args, cap_cli_args * sizeof(CliArg));
            }
            cli_args[n_cli_args].name = argv[i + 1];
            cli_args[n_cli_args].value = asnix_eval_expr_source(argv[i + 2]);
            n_cli_args++;
            i += 2;
        } else if (strcmp(argv[i], "--argstr") == 0 && i + 2 < argc) {
            if (n_cli_args == cap_cli_args) {
                cap_cli_args = cap_cli_args ? cap_cli_args * 2 : 4;
                cli_args = realloc(cli_args, cap_cli_args * sizeof(CliArg));
            }
            cli_args[n_cli_args].name = argv[i + 1];
            cli_args[n_cli_args].value = asnix_make_string_value(argv[i + 2], strlen(argv[i + 2]));
            n_cli_args++;
            i += 2;
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            attrpath = argv[i + 1];
            i += 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                    "asnix: unrecognized flag '%s' (only --arg/--argstr/-A/--flake/-E/--expr/--json/--raw are "
                    "supported)\n",
                    argv[i]);
            return 1;
        } else if (file_path) {
            fprintf(stderr,
                    "usage: %s [--arg name expr] [--argstr name str] [-A attrpath] [--json|--raw] <file.nix>\n",
                    argv[0]);
            return 1;
        } else {
            file_path = argv[i];
        }
    }
    if ((flake_ref ? 1 : 0) + (file_path ? 1 : 0) + (expr_source ? 1 : 0) > 1) {
        fprintf(stderr, "asnix: --flake, -E/--expr, and a plain <file.nix> argument are mutually exclusive\n");
        return 1;
    }
    if (want_json && want_raw) {
        fprintf(stderr, "asnix: --json and --raw are mutually exclusive\n");
        return 1;
    }
    if (!file_path && !flake_ref && !expr_source) {
        fprintf(stderr,
                "usage: %s [--arg name expr] [--argstr name str] [-A attrpath] [--json|--raw] <file.nix>\n"
                "       %s [--arg name expr] [--argstr name str] [-A attrpath] [--json|--raw] -E <expr>\n"
                "       %s --flake <dir>[#attr.path] [-A attrpath] [--json|--raw]\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    Value result;
    if (expr_source) {
        result = asnix_eval_expr_source(expr_source);
    } else if (flake_ref) {
        const char *hash = strchr(flake_ref, '#');
        char dir_buf[1024];
        const char *fragment = NULL;
        if (hash) {
            size_t dirlen = (size_t)(hash - flake_ref);
            if (dirlen >= sizeof(dir_buf)) {
                fprintf(stderr, "asnix: --flake: path '%s' is too long\n", flake_ref);
                return 1;
            }
            memcpy(dir_buf, flake_ref, dirlen);
            dir_buf[dirlen] = '\0';
            if (hash[1]) fragment = hash + 1;
        } else {
            snprintf(dir_buf, sizeof(dir_buf), "%s", flake_ref);
        }
        result = asnix_eval_flake_outputs(dir_buf);
        if (fragment) result = select_attrpath(result, fragment, cli_args, n_cli_args);
    } else {
        result = asnix_eval_file(file_path);
    }
    if (attrpath) result = select_attrpath(result, attrpath, cli_args, n_cli_args);
    /* Separate from select_attrpath's own per-segment auto-call above:
     * real Nix's `nix-instantiate --eval` auto-calls the fully-resolved
     * TERMINAL value too, but ONLY when at least one --arg/--argstr was
     * actually given (confirmed: `-A f` on `{ f = x: x; }` with no
     * --arg prints the function itself, uncalled) -- gated the same
     * way here, not unconditional like the per-segment case above. */
    if (n_cli_args > 0) result = auto_call(result, cli_args, n_cli_args);
    if (want_json) {
        char *json = asnix_toJSON_cstr(result);
        printf("%s\n", json);
        free(json);
    } else if (want_raw) {
        Value coerced = asnix_coerce_to_string(asnix_force_for_print(result));
        int64_t len;
        const char *bytes = asnix_ptr_bytes(coerced, &len);
        fwrite(bytes, 1, (size_t)len, stdout);
    } else {
        print_value(result);
    }
    if (getenv("ASNIX_SHOW_STATS")) asnix_stats_print();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_print_stats(stderr);
    return 0;
}

