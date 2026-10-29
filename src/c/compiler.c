#define _DEFAULT_SOURCE
#include "compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "builtins_list2.h"
#include "builtins_m23.h"
#include "builtins_regex.h"
#include "builtins_shared.h"
#include "eval_file.h"
#include "parser.h"
#include "symtab.h"
#include "tryeval.h"
#include "value_util.h"

static const char *g_compile_base_dir = NULL;

static const char *g_compile_file_path = NULL;

#define canon_path asnix_canon_path

static char *resolve_search_path(const char *name, size_t name_len) {
    const char *nix_path = getenv("NIX_PATH");
    const char *first_slash = memchr(name, '/', name_len);
    size_t head_len = first_slash ? (size_t)(first_slash - name) : name_len;

    if (nix_path && nix_path[0]) {
        char *copy = malloc(strlen(nix_path) + 1);
        memcpy(copy, nix_path, strlen(nix_path) + 1);
        char *entry = copy;
        while (entry) {
            char *colon = strchr(entry, ':');
            if (colon) *colon = '\0';
            if (*entry) {
                char *eq = strchr(entry, '=');
                const char *dir;
                int matched = 0;
                if (eq) {
                    size_t ename_len = (size_t)(eq - entry);
                    if (ename_len == head_len && memcmp(entry, name, head_len) == 0) matched = 1;
                    dir = eq + 1;
                } else {
                    dir = entry;
                    matched = 1;
                }
                if (matched) {
                    size_t dir_len = strlen(dir);
                    size_t cand_len = dir_len + 1 + name_len;
                    char *cand = malloc(cand_len + 1);
                    memcpy(cand, dir, dir_len);
                    cand[dir_len] = '/';
                    memcpy(cand + dir_len + 1, name, name_len);
                    cand[cand_len] = '\0';
                    if (eq) return cand;
                    struct stat st;
                    if (stat(cand, &st) == 0) return cand;
                    free(cand);
                }
            }
            entry = colon ? colon + 1 : NULL;
        }
        free(copy);
    }

    static const char fmt[] = "asnix: file '%.*s' was not found in the Nix search path (set $NIX_PATH)\n";
    int needed = snprintf(NULL, 0, fmt, (int)name_len, name);
    char *formatted = malloc((size_t)needed + 1);
    snprintf(formatted, (size_t)needed + 1, fmt, (int)name_len, name);
    asnix_tryeval_raise_or_fatal(formatted, (size_t)needed);
}

static const char ASNIX_FETCHURL_NIX_SOURCE[] =
    "{\n"
    "  system ? \"\", # obsolete\n"
    "  url,\n"
    "  hash ? \"\", # an SRI hash\n"
    "\n"
    "  # Legacy hash specification\n"
    "  md5 ? \"\",\n"
    "  sha1 ? \"\",\n"
    "  sha256 ? \"\",\n"
    "  sha512 ? \"\",\n"
    "  outputHash ?\n"
    "    if hash != \"\" then\n"
    "      hash\n"
    "    else if sha512 != \"\" then\n"
    "      sha512\n"
    "    else if sha1 != \"\" then\n"
    "      sha1\n"
    "    else if md5 != \"\" then\n"
    "      md5\n"
    "    else\n"
    "      sha256,\n"
    "  outputHashAlgo ?\n"
    "    if hash != \"\" then\n"
    "      \"\"\n"
    "    else if sha512 != \"\" then\n"
    "      \"sha512\"\n"
    "    else if sha1 != \"\" then\n"
    "      \"sha1\"\n"
    "    else if md5 != \"\" then\n"
    "      \"md5\"\n"
    "    else\n"
    "      \"sha256\",\n"
    "\n"
    "  executable ? false,\n"
    "  unpack ? false,\n"
    "  name ? baseNameOf (toString url),\n"
    "  impure ? false,\n"
    "}:\n"
    "\n"
    "derivation (\n"
    "  {\n"
    "    builder = \"builtin:fetchurl\";\n"
    "\n"
    "    # New-style output content requirements.\n"
    "    outputHashMode = if unpack || executable then \"recursive\" else \"flat\";\n"
    "\n"
    "    inherit\n"
    "      name\n"
    "      url\n"
    "      executable\n"
    "      unpack\n"
    "      ;\n"
    "\n"
    "    system = \"builtin\";\n"
    "\n"
    "    # No need to double the amount of network traffic\n"
    "    preferLocalBuild = true;\n"
    "\n"
    "    # This attribute does nothing; it's here to avoid changing evaluation results.\n"
    "    impureEnvVars = [\n"
    "      \"http_proxy\"\n"
    "      \"https_proxy\"\n"
    "      \"ftp_proxy\"\n"
    "      \"all_proxy\"\n"
    "      \"no_proxy\"\n"
    "    ];\n"
    "\n"
    "    # To make \"nix-prefetch-url\" work.\n"
    "    urls = [ url ];\n"
    "  }\n"
    "  // (if impure then { __impure = true; } else { inherit outputHashAlgo outputHash; })\n"
    ")\n";

#define ASNIX_FETCHURL_TEMP_PATH "/tmp/.asnix-corepkgs-fetchurl.nix"

static const char *write_fetchurl_nix(void) {
    static int written = 0;
    if (written) return ASNIX_FETCHURL_TEMP_PATH;
    FILE *f = fopen(ASNIX_FETCHURL_TEMP_PATH, "w");
    if (!f) {
        fprintf(stderr, "asnix: could not write '%s' for <nix/fetchurl.nix>\n", ASNIX_FETCHURL_TEMP_PATH);
        exit(1);
    }
    fwrite(ASNIX_FETCHURL_NIX_SOURCE, 1, sizeof(ASNIX_FETCHURL_NIX_SOURCE) - 1, f);
    fclose(f);
    written = 1;
    return ASNIX_FETCHURL_TEMP_PATH;
}

Value asnix_resolve_search_path_value(Value name_val) {
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    if (len == 16 && memcmp(bytes, "nix/fetchurl.nix", 16) == 0) {
        const char *path = write_fetchurl_nix();
        return asnix_make_path_value(path, strlen(path));
    }
    char *resolved = canon_path(resolve_search_path(bytes, (size_t)len));
    Value v = asnix_make_path_value(resolved, strlen(resolved));
    free(resolved);
    return v;
}

static char *resolve_home_literal(const char *text, size_t len) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "asnix: cannot resolve '~' -- $HOME is not set\n");
        exit(1);
    }
    size_t home_len = strlen(home);
    size_t rest_len = len - 1;
    char *joined = malloc(home_len + rest_len + 1);
    memcpy(joined, home, home_len);
    memcpy(joined + home_len, text + 1, rest_len);
    joined[home_len + rest_len] = '\0';
    return joined;
}

static char *resolve_path_literal(Ast *node) {
    const char *text = node->as.path.text;
    size_t len = node->as.path.len;

    if (len > 0 && text[len - 1] == '/') {
        fprintf(stderr, "asnix: path has a trailing slash\n");
        exit(1);
    }

    if (node->as.path.is_home) {
        return resolve_home_literal(text, len);
    }

    char *joined;
    if (len > 0 && text[0] == '/') {
        joined = malloc(len + 1);
        memcpy(joined, text, len);
        joined[len] = '\0';
    } else {
        size_t base_len = strlen(g_compile_base_dir);
        joined = malloc(base_len + 1 + len + 1);
        memcpy(joined, g_compile_base_dir, base_len);
        joined[base_len] = '/';
        memcpy(joined + base_len + 1, text, len);
        joined[base_len + 1 + len] = '\0';
    }
    char *canon = canon_path(joined);
    free(joined);
    return canon;
}

static Value resolve_path_chunk(const char *text, size_t len, int is_home) {
    if (is_home) {
        char *joined = resolve_home_literal(text, len);
        Value v = asnix_make_path_value(joined, strlen(joined));
        free(joined);
        return v;
    }
    int had_trailing_slash = (len > 1 && text[len - 1] == '/');
    char *joined;
    if (len > 0 && text[0] == '/') {
        joined = malloc(len + 1);
        memcpy(joined, text, len);
        joined[len] = '\0';
    } else {
        size_t base_len = strlen(g_compile_base_dir);
        joined = malloc(base_len + 1 + len + 1);
        memcpy(joined, g_compile_base_dir, base_len);
        joined[base_len] = '/';
        memcpy(joined + base_len + 1, text, len);
        joined[base_len + 1 + len] = '\0';
    }
    char *canon = canon_path(joined);
    free(joined);
    if (had_trailing_slash) {
        size_t clen = strlen(canon);
        char *with_slash = malloc(clen + 2);
        memcpy(with_slash, canon, clen);
        with_slash[clen] = '/';
        with_slash[clen + 1] = '\0';
        free(canon);
        canon = with_slash;
    }
    Value v = asnix_make_path_value(canon, strlen(canon));
    free(canon);
    return v;
}

typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char **names;
    size_t *name_lens;
    size_t count;
    int is_with;
    int has_overrides;
} ScopeFrame;

int asnix_lazy_thunk_enabled = 0;

typedef struct LazyThunkDescriptor {
    IrProgram *self;
    Ast *body;
    ScopeFrame *scope;
    const char *file_path;
    const char *base_dir;
    uint32_t value_src_pos;
} LazyThunkDescriptor;

static ScopeFrame *scope_frame_persist(ScopeFrame *stack_scope) {
    if (!stack_scope) return NULL;
    ScopeFrame *copy = malloc(sizeof(ScopeFrame));
    *copy = *stack_scope;
    copy->up = scope_frame_persist(stack_scope->up);
    if (stack_scope->count > 0) {
        const char **names = malloc(stack_scope->count * sizeof(char *));
        size_t *lens = malloc(stack_scope->count * sizeof(size_t));
        memcpy(names, stack_scope->names, stack_scope->count * sizeof(char *));
        memcpy(lens, stack_scope->name_lens, stack_scope->count * sizeof(size_t));
        copy->names = names;
        copy->name_lens = lens;
    }
    return copy;
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope);

typedef struct ReparseSrcEntry {
    char *path;
    char *src;
    struct ReparseSrcEntry *next;
} ReparseSrcEntry;
static ReparseSrcEntry *g_reparse_src_cache = NULL;

static const char *reparse_source_for_file(const char *path) {
    for (ReparseSrcEntry *e = g_reparse_src_cache; e; e = e->next) {
        if (strcmp(e->path, path) == 0) return e->src;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: cannot reopen '%s' to force a cache-loaded lazy thunk\n", path);
        exit(1);
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        fprintf(stderr, "asnix: cannot reopen '%s' to force a cache-loaded lazy thunk\n", path);
        exit(1);
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    buf[got] = '\0';
    ReparseSrcEntry *e = malloc(sizeof(ReparseSrcEntry));
    e->path = strdup(path);
    e->src = buf;
    e->next = g_reparse_src_cache;
    g_reparse_src_cache = e;
    return buf;
}

IrProgram *asnix_compile_lazy_thunk_body(void *desc_void) {
    LazyThunkDescriptor *desc = (LazyThunkDescriptor *)desc_void;
    const char *saved_base_dir = g_compile_base_dir;
    const char *saved_file_path = g_compile_file_path;
    g_compile_base_dir = desc->base_dir;
    g_compile_file_path = desc->file_path;

    Ast *body = desc->body;
    if (!body) {
        const char *src = reparse_source_for_file(desc->file_path);
        body = asnix_parse_subexpr(src, desc->value_src_pos);
    }

    IrBuilder *real_b = ib_new();
    compile_expr(real_b, body, desc->scope);
    ib_ret(real_b);
    IrProgram *real = ib_finish(real_b);

    g_compile_base_dir = saved_base_dir;
    g_compile_file_path = saved_file_path;

    desc->self->ops = real->ops;
    desc->self->pool = real->pool;
    free(real);
    return desc->self;
}

int asnix_lazy_thunk_descriptor_src_pos(void *desc_void, unsigned int *out_pos) {
    LazyThunkDescriptor *desc = (LazyThunkDescriptor *)desc_void;
    if (desc->value_src_pos == 0) return 0;
    *out_pos = desc->value_src_pos;
    return 1;
}

const char *asnix_lazy_thunk_descriptor_file_path(void *desc_void) {
    return ((LazyThunkDescriptor *)desc_void)->file_path;
}

const char *asnix_lazy_thunk_descriptor_base_dir(void *desc_void) {
    return ((LazyThunkDescriptor *)desc_void)->base_dir;
}

void *asnix_lazy_thunk_descriptor_scope(void *desc_void) {
    return ((LazyThunkDescriptor *)desc_void)->scope;
}

void *asnix_scope_frame_up(void *frame) {
    return frame ? ((ScopeFrame *)frame)->up : NULL;
}

int asnix_scope_frame_count(void *frame) {
    return (int)((ScopeFrame *)frame)->count;
}

int asnix_scope_frame_is_with(void *frame) {
    return ((ScopeFrame *)frame)->is_with;
}

int asnix_scope_frame_has_overrides(void *frame) {
    return ((ScopeFrame *)frame)->has_overrides;
}

const char *asnix_scope_frame_name(void *frame, int i, size_t *out_len) {
    ScopeFrame *f = (ScopeFrame *)frame;
    *out_len = f->name_lens[i];
    return f->names[i];
}

void *asnix_lazy_scope_push_frame(void *up_chain, int count, int is_with, int has_overrides,
                                   const char **names, const size_t *name_lens) {
    ScopeFrame *f = malloc(sizeof(ScopeFrame));
    f->up = (ScopeFrame *)up_chain;
    f->count = (size_t)count;
    f->is_with = is_with;
    f->has_overrides = has_overrides;
    f->names = NULL;
    f->name_lens = NULL;
    if (count > 0) {
        const char **n = malloc((size_t)count * sizeof(char *));
        size_t *l = malloc((size_t)count * sizeof(size_t));
        for (int i = 0; i < count; i++) {
            size_t len = name_lens[i];
            char *copy = malloc(len + 1);
            memcpy(copy, names[i], len);
            copy[len] = '\0';
            n[i] = copy;
            l[i] = len;
        }
        f->names = n;
        f->name_lens = l;
    }
    return f;
}

void *asnix_make_cached_lazy_thunk_descriptor(unsigned int reparse_pos, const char *file_path,
                                               const char *base_dir, void *scope_chain) {
    LazyThunkDescriptor *desc = malloc(sizeof(LazyThunkDescriptor));
    desc->self = NULL;
    desc->body = NULL;
    desc->scope = (ScopeFrame *)scope_chain;
    desc->file_path = file_path;
    desc->base_dir = base_dir;
    desc->value_src_pos = reparse_pos;
    return desc;
}

void asnix_lazy_thunk_descriptor_set_self(void *desc_void, IrProgram *self) {
    ((LazyThunkDescriptor *)desc_void)->self = self;
}

static void emit_attrset_field_value(IrBuilder *b, Ast *value, ScopeFrame *value_scope) {
    if (!asnix_lazy_thunk_enabled || value->src_pos == 0) {
        IrBuilder *thunk_b = ib_new();
        compile_expr(thunk_b, value, value_scope);
        ib_ret(thunk_b);
        ib_mk_thunk(b, ib_finish(thunk_b));
        return;
    }
    LazyThunkDescriptor *desc = malloc(sizeof(LazyThunkDescriptor));
    desc->body = value;
    desc->scope = scope_frame_persist(value_scope);
    desc->file_path = g_compile_file_path;
    desc->base_dir = g_compile_base_dir;
    desc->value_src_pos = value->src_pos;
    IrBuilder *stub_b = ib_new();
    ib_mk_lazy_thunk_stub(stub_b, desc);
    IrProgram *stub_prog = ib_finish(stub_b);
    desc->self = stub_prog;
    ib_mk_thunk(b, stub_prog);
}

#define ASNIX_MAX_WITH_CHAIN 64

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    if (a == NULL || alen != blen) return 0;
    if (alen == 0) return 1;
    if (a[0] != b[0]) return 0;
    return memcmp(a, b, alen) == 0;
}

static int scope_find_lexical(ScopeFrame *scope, const char *name, size_t name_len, int *out_depth,
                               int64_t *out_slot, int *out_has_overrides) {
    int depth = 0;
    for (ScopeFrame *f = scope; f; f = f->up, depth++) {
        for (size_t slot = 0; slot < f->count; slot++) {
            if (name_eq(f->names[slot], f->name_lens[slot], name, name_len)) {
                *out_depth = depth;
                *out_slot = (int64_t)slot;
                if (out_has_overrides) *out_has_overrides = f->has_overrides;
                return 1;
            }
        }
    }
    return 0;
}

static int try_cell_ref(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    if (node->kind != AST_VAR) return 0;
    int depth;
    int64_t slot;
    int has_overrides = 0;
    if (!scope_find_lexical(scope, node->as.var.name, node->as.var.name_len, &depth, &slot, &has_overrides))
        return 0;
    if (has_overrides) return 0;
    ib_var_ref_cell(b, depth, slot);
    return 1;
}

static void sort_syms(uint32_t *syms, size_t n) {
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t tmp = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = tmp;
            j--;
        }
    }
}

typedef struct {
    uint32_t sym;
    size_t idx;
} SymIdxPair;

static SymIdxPair *merge_sort_symidxpairs(SymIdxPair *a, SymIdxPair *tmp, size_t n) {
    SymIdxPair *src = a, *dst = tmp;
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t i = 0; i < n; i += 2 * width) {
            size_t mid = i + width < n ? i + width : n;
            size_t end = i + 2 * width < n ? i + 2 * width : n;
            size_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (src[l].sym <= src[r].sym) dst[k++] = src[l++];
                else dst[k++] = src[r++];
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < end) dst[k++] = src[r++];
        }
        SymIdxPair *t = src; src = dst; dst = t;
    }
    return src;
}
#define ASNIX_COMPILE_SCRATCH_MAX 32
#define ASNIX_ATTRSET_SORT_INSERTION_MAX ASNIX_COMPILE_SCRATCH_MAX
static void sort_attrset_bindings_large(uint32_t *syms, size_t *order, size_t n) {
    SymIdxPair *pairs = asnix_arena_alloc(n * sizeof(SymIdxPair));
    SymIdxPair *scratch = asnix_arena_alloc(n * sizeof(SymIdxPair));
    for (size_t i = 0; i < n; i++) { pairs[i].sym = syms[i]; pairs[i].idx = i; }
    SymIdxPair *sorted = merge_sort_symidxpairs(pairs, scratch, n);
    for (size_t i = 0; i < n; i++) order[i] = sorted[i].idx;
}

static int try_fold_const_binop(Ast *node, int *out_is_bool, int *out_is_float, int64_t *out_val,
                                 double *out_fval) {
    if (node->kind != AST_BINOP) return 0;
    Ast *lhs = node->as.binop.lhs, *rhs = node->as.binop.rhs;
    int lhs_is_int = (lhs->kind == AST_INT), rhs_is_int = (rhs->kind == AST_INT);
    int lhs_is_float = (lhs->kind == AST_FLOAT), rhs_is_float = (rhs->kind == AST_FLOAT);
    if (!(lhs_is_int || lhs_is_float) || !(rhs_is_int || rhs_is_float)) return 0;

    if (lhs_is_int && rhs_is_int) {
        int64_t a = lhs->as.int_val, b = rhs->as.int_val, result;
        switch (node->as.binop.op) {
            case '+':
                if (__builtin_add_overflow(a, b, &result)) return 0;
                break;
            case '-':
                if (__builtin_sub_overflow(a, b, &result)) return 0;
                break;
            case '*':
                if (__builtin_mul_overflow(a, b, &result)) return 0;
                break;
            case '/':
                if (b == 0) return 0;
                if (a == INT64_MIN && b == -1) return 0;
                result = a / b;
                break;
            case 'e': *out_is_bool = 1; *out_is_float = 0; *out_val = (a == b); return 1;
            case 'n': *out_is_bool = 1; *out_is_float = 0; *out_val = (a != b); return 1;
            case 'l': *out_is_bool = 1; *out_is_float = 0; *out_val = (a < b); return 1;
            case 'G': *out_is_bool = 1; *out_is_float = 0; *out_val = (a >= b); return 1;
            case 'g': *out_is_bool = 1; *out_is_float = 0; *out_val = (a > b); return 1;
            case 'L': *out_is_bool = 1; *out_is_float = 0; *out_val = (a <= b); return 1;
            default: return 0;
        }
        if (result < ASNIX_INT_MIN || result > ASNIX_INT_MAX) return 0;
        *out_is_bool = 0;
        *out_is_float = 0;
        *out_val = result;
        return 1;
    }

    double a = lhs_is_float ? lhs->as.float_val : (double)lhs->as.int_val;
    double b = rhs_is_float ? rhs->as.float_val : (double)rhs->as.int_val;
    switch (node->as.binop.op) {
        case '+': *out_is_bool = 0; *out_is_float = 1; *out_fval = a + b; return 1;
        case '-': *out_is_bool = 0; *out_is_float = 1; *out_fval = a - b; return 1;
        case '*': *out_is_bool = 0; *out_is_float = 1; *out_fval = a * b; return 1;
        case '/':
            if (b == 0.0) return 0;
            *out_is_bool = 0;
            *out_is_float = 1;
            *out_fval = a / b;
            return 1;
        case 'e': *out_is_bool = 1; *out_is_float = 0; *out_val = (a == b); return 1;
        case 'n': *out_is_bool = 1; *out_is_float = 0; *out_val = (a != b); return 1;
        case 'l': *out_is_bool = 1; *out_is_float = 0; *out_val = (a < b); return 1;
        case 'G': *out_is_bool = 1; *out_is_float = 0; *out_val = (a >= b); return 1;
        case 'g': *out_is_bool = 1; *out_is_float = 0; *out_val = (a > b); return 1;
        case 'L': *out_is_bool = 1; *out_is_float = 0; *out_val = (a <= b); return 1;
        default: return 0;
    }
}

static int is_thunk_free_literal(Ast *node) {
    switch (node->kind) {
        case AST_INT:
            return node->as.int_val >= ASNIX_INT_MIN && node->as.int_val <= ASNIX_INT_MAX;
        case AST_FLOAT:
        case AST_BOOL:
        case AST_NULL:
        case AST_CURPOS:
            return 1;
        case AST_BINOP: {
            int is_bool, is_float;
            int64_t val;
            double fval;
            return try_fold_const_binop(node, &is_bool, &is_float, &val, &fval);
        }
        case AST_STRING:
            for (size_t i = 0; i < node->as.string.count; i++) {
                if (node->as.string.parts[i].is_expr) return 0;
            }
            return 1;
        case AST_PATH:
            return !node->as.path.is_search && node->as.path.interp_count == 0;
        case AST_LIST:
            return node->as.list.count == 0;
        default:
            return 0;
    }
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope);

typedef struct {
    const char *name;
    size_t name_len;
    void *fn;
    int noforce;
} FusedNative2;

static const FusedNative2 FUSED_NATIVES_2[] = {
    { "elem", 4, (void *)asnix_prim_elem2_impl, 1 },
    { "any", 3, (void *)asnix_prim_any2_impl, 0 },
    { "all", 3, (void *)asnix_prim_all2_impl, 0 },
    { "match", 5, (void *)asnix_prim_match2_impl, 0 },
    { "intersectAttrs", 15, (void *)asnix_prim_intersectAttrs2_impl, 0 },
};
#define FUSED_NATIVES_2_COUNT (sizeof(FUSED_NATIVES_2) / sizeof(FUSED_NATIVES_2[0]))

static int try_fused_native_call2(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    if (node->kind != AST_APP) return 0;
    Ast *inner = node->as.app.fn;
    if (inner->kind != AST_APP) return 0;
    Ast *sel = inner->as.app.fn;
    if (sel->kind != AST_SELECT) return 0;
    if (sel->as.select.attr_expr != NULL) return 0;
    if (sel->as.select.default_ != NULL) return 0;
    Ast *builtins_var = sel->as.select.target;
    if (builtins_var->kind != AST_VAR) return 0;
    if (!name_eq(builtins_var->as.var.name, builtins_var->as.var.name_len, "builtins", 8)) return 0;

    const FusedNative2 *entry = NULL;
    for (size_t i = 0; i < FUSED_NATIVES_2_COUNT; i++) {
        if (name_eq(sel->as.select.attr, sel->as.select.attr_len, FUSED_NATIVES_2[i].name,
                    FUSED_NATIVES_2[i].name_len)) {
            entry = &FUSED_NATIVES_2[i];
            break;
        }
    }
    if (entry == NULL) return 0;

    int depth;
    int64_t slot;
    if (!scope_find_lexical(scope, builtins_var->as.var.name, builtins_var->as.var.name_len, &depth, &slot, NULL))
        return 0;
    ScopeFrame *f = scope;
    for (int i = 0; i < depth; i++) f = f->up;
    if (f->up != NULL) return 0;

    Ast *arg1 = inner->as.app.arg;
    Ast *arg2 = node->as.app.arg;
    Value native_primop = entry->noforce ? asnix_bshared_make_primop_noforce(entry->fn, 2)
                                          : asnix_bshared_make_primop(entry->fn, 2);
    ib_push_const(b, (int64_t)native_primop.raw);
    if (!try_cell_ref(b, arg1, scope)) {
        if (is_thunk_free_literal(arg1)) {
            compile_expr(b, arg1, scope);
        } else {
            IrBuilder *arg_b = ib_new();
            compile_expr(arg_b, arg1, scope);
            ib_ret(arg_b);
            ib_mk_thunk(b, ib_finish(arg_b));
        }
    }
    if (!try_cell_ref(b, arg2, scope)) {
        if (is_thunk_free_literal(arg2)) {
            compile_expr(b, arg2, scope);
        } else {
            IrBuilder *arg_b = ib_new();
            compile_expr(arg_b, arg2, scope);
            ib_ret(arg_b);
            ib_mk_thunk(b, ib_finish(arg_b));
        }
    }
    ib_call(b, 2);
    return 1;
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    ib_set_current_src_pos(node->src_pos);
    switch (node->kind) {
        case AST_INT:
            ib_push_int(b, node->as.int_val);
            return;
        case AST_FLOAT: {
            Value fv = asnix_make_float_value(node->as.float_val);
            ib_push_const(b, (int64_t)fv.raw);
            return;
        }
        case AST_BOOL:
            if (node->as.bool_val)
                ib_push_true(b);
            else
                ib_push_false(b);
            return;
        case AST_NULL:
            ib_push_null(b);
            return;
        case AST_VAR: {
            int depth = 0;
            int64_t with_depths[ASNIX_MAX_WITH_CHAIN];
            size_t with_count = 0;
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                for (size_t slot = 0; slot < f->count; slot++) {
                    if (name_eq(f->names[slot], f->name_lens[slot], node->as.var.name,
                                node->as.var.name_len)) {
                        ib_var_ref(b, depth, (int64_t)slot);
                        return;
                    }
                }
                if (f->is_with) {
                    if (with_count >= ASNIX_MAX_WITH_CHAIN) {
                        fprintf(stderr, "asnix: too many nested `with` scopes (limit %d)\n",
                                ASNIX_MAX_WITH_CHAIN);
                        exit(1);
                    }
                    with_depths[with_count++] = depth;
                }
            }
            if (with_count == 0) {
                fprintf(stderr, "asnix: undefined variable '%.*s'\n", (int)node->as.var.name_len,
                        node->as.var.name);
                exit(1);
            }
            uint32_t sym = asnix_intern(node->as.var.name, node->as.var.name_len);
            size_t jmp_sites[ASNIX_MAX_WITH_CHAIN];
            for (size_t i = 0; i < with_count; i++) {
                ib_var_ref(b, with_depths[i], 0);
                size_t try_site = ib_with_try(b, sym);
                jmp_sites[i] = ib_jmp(b);
                size_t next_idx = ib_len(b);
                ib_patch_delta(b, try_site, next_idx);
            }
            ib_with_miss(b);
            size_t end_idx = ib_len(b);
            for (size_t i = 0; i < with_count; i++) ib_patch_delta(b, jmp_sites[i], end_idx);
            return;
        }
        case AST_LET: {
            size_t n = node->as.let1.count;
            AstBinding *bindings = node->as.let1.bindings;
            const char *names_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
            int names_on_stack = (n <= ASNIX_COMPILE_SCRATCH_MAX);
            const char **names = names_on_stack ? names_stack : malloc(n * sizeof(char *));
            size_t *name_lens = names_on_stack ? name_lens_stack : malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                names[i] = bindings[i].name;
                name_lens[i] = bindings[i].name_len;
            }
            ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
            ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

            ib_frame_alloc(b, (int64_t)n);
            for (size_t i = 0; i < n; i++) {
                ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                if (try_cell_ref(b, bindings[i].value, value_scope)) {
                } else if (is_thunk_free_literal(bindings[i].value)) {
                    compile_expr(b, bindings[i].value, value_scope);
                } else {
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, bindings[i].value, value_scope);
                    ib_ret(thunk_b);
                    ib_mk_thunk(b, ib_finish(thunk_b));
                }
                ib_frame_set(b, (int64_t)i);
            }
            compile_expr(b, node->as.let1.body, &frame);
            ib_frame_exit(b);
            if (!names_on_stack) { free(names); free(name_lens); }
            return;
        }
        case AST_IF: {
            Ast *cond = node->as.if1.cond;
            Ast *then_node = node->as.if1.then_;
            Ast *else_node = node->as.if1.else_;

            if (cond->kind == AST_HAS_ATTR && !cond->as.has_attr.attr_expr) {
                compile_expr(b, cond->as.has_attr.target, scope);
                uint32_t sym = asnix_intern(cond->as.has_attr.attr, cond->as.has_attr.attr_len);
                size_t if_site = ib_has_attr_if(b, sym);
                compile_expr(b, then_node, scope);
                size_t jmp_site = ib_jmp(b);
                size_t else_idx = ib_len(b);
                ib_patch_delta(b, if_site, else_idx);
                compile_expr(b, else_node, scope);
                size_t end_idx = ib_len(b);
                ib_patch_delta(b, jmp_site, end_idx);
                return;
            }
            if (cond->kind == AST_BINOP) {
                char cop = cond->as.binop.op;
                int is_cmp = (cop == 'e' || cop == 'n' || cop == 'l' || cop == 'G' || cop == 'g' || cop == 'L');
                int fold_is_bool, fold_is_float;
                int64_t fold_val;
                double fold_fval;
                if (is_cmp && !try_fold_const_binop(cond, &fold_is_bool, &fold_is_float, &fold_val, &fold_fval)) {
                    size_t if_site;
                    switch (cop) {
                        case 'e':
                        case 'n':
                        case 'l':
                        case 'G':
                            compile_expr(b, cond->as.binop.lhs, scope);
                            compile_expr(b, cond->as.binop.rhs, scope);
                            if (cop == 'e') if_site = ib_eq_if(b);
                            else if (cop == 'n') if_site = ib_neq_if(b);
                            else if (cop == 'l') if_site = ib_lt_if(b);
                            else if_site = ib_ge_if(b);
                            compile_expr(b, then_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t else_idx = ib_len(b);
                                ib_patch_delta(b, if_site, else_idx);
                                compile_expr(b, else_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                        case 'g':
                            compile_expr(b, cond->as.binop.rhs, scope);
                            compile_expr(b, cond->as.binop.lhs, scope);
                            if_site = ib_lt_if(b);
                            compile_expr(b, then_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t else_idx = ib_len(b);
                                ib_patch_delta(b, if_site, else_idx);
                                compile_expr(b, else_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                        case 'L':
                            compile_expr(b, cond->as.binop.rhs, scope);
                            compile_expr(b, cond->as.binop.lhs, scope);
                            if_site = ib_lt_if(b);
                            compile_expr(b, else_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t then_idx = ib_len(b);
                                ib_patch_delta(b, if_site, then_idx);
                                compile_expr(b, then_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                    }
                }
            }

            compile_expr(b, cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, then_node, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, else_node, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_LAMBDA: {
            if (!node->as.lambda.is_pattern) {
                IrBuilder *body_b = ib_new();
                const char *names[1] = { node->as.lambda.param };
                size_t name_lens[1] = { node->as.lambda.param_len };
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = 1 };
                compile_expr(body_b, node->as.lambda.body, &frame);
                ib_ret(body_b);
                IrProgram *body_prog = ib_finish(body_b);
                ib_lambda(b, 1, body_prog, (int64_t)asnix_bshared_make_attrs(NULL, NULL, 0).raw);
                return;
            }

            size_t nf = node->as.lambda.formals_count;
            IrBuilder *body_b = ib_new();

            const char *arg_names[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name : "" };
            size_t arg_name_lens[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name_len : 0 };
            ScopeFrame argframe = { .up = scope, .names = arg_names, .name_lens = arg_name_lens, .count = 1 };

            int nf_on_stack = (nf <= ASNIX_COMPILE_SCRATCH_MAX);
            uint32_t all_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t required_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t all_syms_sorted_stack[ASNIX_COMPILE_SCRATCH_MAX];
            const char *field_names_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t field_name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t fa_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            Value fa_vals_stack[ASNIX_COMPILE_SCRATCH_MAX];

            uint32_t *all_syms = nf_on_stack ? all_syms_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
            size_t required_count = 0;
            for (size_t i = 0; i < nf; i++) {
                all_syms[i] = asnix_intern(node->as.lambda.formals[i].name, node->as.lambda.formals[i].name_len);
                if (!node->as.lambda.formals[i].default_) required_count++;
            }
            uint32_t *required_syms = nf_on_stack ? required_syms_stack : malloc((required_count ? required_count : 1) * sizeof(uint32_t));
            {
                size_t k = 0;
                for (size_t i = 0; i < nf; i++) {
                    if (!node->as.lambda.formals[i].default_) required_syms[k++] = all_syms[i];
                }
            }
            sort_syms(required_syms, required_count);
            uint32_t *all_syms_sorted = NULL;
            int64_t total_count = -1;
            if (!node->as.lambda.has_ellipsis) {
                all_syms_sorted = nf_on_stack ? all_syms_sorted_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
                memcpy(all_syms_sorted, all_syms, nf * sizeof(uint32_t));
                sort_syms(all_syms_sorted, nf);
                total_count = (int64_t)nf;
            }

            ib_var_ref(body_b, 0, 0);
            ib_check_formals(body_b, (int64_t)required_count, required_syms, total_count, all_syms_sorted);
            if (!nf_on_stack) {
                free(all_syms);
                free(required_syms);
                free(all_syms_sorted);
            }

            ib_frame_alloc(body_b, (int64_t)nf);

            const char **field_names = nf_on_stack ? field_names_stack : malloc((nf ? nf : 1) * sizeof(char *));
            size_t *field_name_lens = nf_on_stack ? field_name_lens_stack : malloc((nf ? nf : 1) * sizeof(size_t));
            for (size_t i = 0; i < nf; i++) {
                field_names[i] = node->as.lambda.formals[i].name;
                field_name_lens[i] = node->as.lambda.formals[i].name_len;
            }
            ScopeFrame fieldframe = { .up = &argframe, .names = field_names, .name_lens = field_name_lens, .count = nf };

            for (size_t i = 0; i < nf; i++) {
                Ast *arg_var = asnix_ast_new(AST_VAR);
                arg_var->as.var.name = arg_names[0];
                arg_var->as.var.name_len = arg_name_lens[0];

                Ast *select_node = asnix_ast_new(AST_SELECT);
                select_node->as.select.target = arg_var;
                select_node->as.select.attr = node->as.lambda.formals[i].name;
                select_node->as.select.attr_len = node->as.lambda.formals[i].name_len;

                Ast *field_body;
                if (node->as.lambda.formals[i].default_ == NULL) {
                    field_body = select_node;
                } else {
                    Ast *arg_var2 = asnix_ast_new(AST_VAR);
                    arg_var2->as.var.name = arg_names[0];
                    arg_var2->as.var.name_len = arg_name_lens[0];
                    Ast *has_node = asnix_ast_new(AST_HAS_ATTR);
                    has_node->as.has_attr.target = arg_var2;
                    has_node->as.has_attr.attr = node->as.lambda.formals[i].name;
                    has_node->as.has_attr.attr_len = node->as.lambda.formals[i].name_len;

                    Ast *if_node = asnix_ast_new(AST_IF);
                    if_node->as.if1.cond = has_node;
                    if_node->as.if1.then_ = select_node;
                    if_node->as.if1.else_ = node->as.lambda.formals[i].default_;
                    field_body = if_node;
                }

                if (try_cell_ref(body_b, field_body, &fieldframe)) {
                } else if (is_thunk_free_literal(field_body)) {
                    compile_expr(body_b, field_body, &fieldframe);
                } else {
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, field_body, &fieldframe);
                    ib_ret(thunk_b);
                    ib_mk_thunk(body_b, ib_finish(thunk_b));
                }
                ib_frame_set(body_b, (int64_t)i);
            }

            compile_expr(body_b, node->as.lambda.body, &fieldframe);
            ib_ret(body_b);
            IrProgram *body_prog = ib_finish(body_b);
            if (!nf_on_stack) {
                free(field_names);
                free(field_name_lens);
            }

            uint32_t *fa_syms = nf_on_stack ? fa_syms_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
            Value *fa_vals = nf_on_stack ? fa_vals_stack : malloc((nf ? nf : 1) * sizeof(Value));
            for (size_t i = 0; i < nf; i++) {
                fa_syms[i] = asnix_intern(node->as.lambda.formals[i].name, node->as.lambda.formals[i].name_len);
                fa_vals[i] = node->as.lambda.formals[i].default_ ? ASNIX_TRUE : ASNIX_FALSE;
            }
            Value function_args = asnix_bshared_make_attrs(fa_syms, fa_vals, (int64_t)nf);
            if (!nf_on_stack) {
                free(fa_syms);
                free(fa_vals);
            }

            ib_lambda(b, 1, body_prog, (int64_t)function_args.raw);
            return;
        }
        case AST_HAS_ATTR: {
            compile_expr(b, node->as.has_attr.target, scope);
            if (node->as.has_attr.attr_expr) {
                compile_expr(b, node->as.has_attr.attr_expr, scope);
                ib_has_attr_dynamic(b);
                return;
            }
            uint32_t sym = asnix_intern(node->as.has_attr.attr, node->as.has_attr.attr_len);
            ib_has_attr(b, sym);
            return;
        }
        case AST_APP: {
            if (try_fused_native_call2(b, node, scope)) return;
            compile_expr(b, node->as.app.fn, scope);
            if (try_cell_ref(b, node->as.app.arg, scope)) {
            } else if (is_thunk_free_literal(node->as.app.arg)) {
                compile_expr(b, node->as.app.arg, scope);
            } else {
                IrBuilder *arg_b = ib_new();
                compile_expr(arg_b, node->as.app.arg, scope);
                ib_ret(arg_b);
                ib_mk_thunk(b, ib_finish(arg_b));
            }
            ib_call(b, 1);
            return;
        }
        case AST_BINOP: {
            int fold_is_bool, fold_is_float;
            int64_t fold_val;
            double fold_fval;
            if (try_fold_const_binop(node, &fold_is_bool, &fold_is_float, &fold_val, &fold_fval)) {
                if (fold_is_bool) {
                    if (fold_val) ib_push_true(b);
                    else ib_push_false(b);
                } else if (fold_is_float) {
                    Value fv = asnix_make_float_value(fold_fval);
                    ib_push_const(b, (int64_t)fv.raw);
                } else {
                    ib_push_int(b, fold_val);
                }
                return;
            }
            if (node->as.binop.op == 'g') {
                compile_expr(b, node->as.binop.rhs, scope);
                compile_expr(b, node->as.binop.lhs, scope);
                ib_lt(b);
                return;
            }
            if (node->as.binop.op == 'L') {
                compile_expr(b, node->as.binop.rhs, scope);
                compile_expr(b, node->as.binop.lhs, scope);
                ib_lt(b);
                ib_not(b);
                return;
            }
            compile_expr(b, node->as.binop.lhs, scope);
            compile_expr(b, node->as.binop.rhs, scope);
            switch (node->as.binop.op) {
                case '+': ib_add(b); return;
                case '-': ib_sub(b); return;
                case '*': ib_mul(b); return;
                case 'e': ib_eq(b); return;
                case 'n': ib_neq(b); return;
                case 'l': ib_lt(b); return;
                case 'G': ib_ge(b); return;
                case 'u': ib_update(b); return;
                case 'c': ib_concat(b); return;
                case '/': ib_div(b); return;
            }
            return;
        }
        case AST_AND: {
            compile_expr(b, node->as.logic.lhs, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.logic.rhs, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            ib_push_false(b);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_OR: {
            compile_expr(b, node->as.logic.lhs, scope);
            size_t if_site = ib_if(b);
            ib_push_true(b);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, node->as.logic.rhs, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_NOT:
            compile_expr(b, node->as.not1.operand, scope);
            ib_not(b);
            return;
        case AST_ASSERT: {
            compile_expr(b, node->as.assert1.cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.assert1.body, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);

            static const char prefix[] = "asnix: assertion '";
            static const char suffix[] = "' failed, aborting\n";
            size_t text_len = node->as.assert1.cond_text_len;
            size_t total = sizeof(prefix) - 1 + text_len + sizeof(suffix) - 1;
            char *msg = asnix_arena_alloc(total);
            memcpy(msg, prefix, sizeof(prefix) - 1);
            memcpy(msg + sizeof(prefix) - 1, node->as.assert1.cond_text, text_len);
            memcpy(msg + sizeof(prefix) - 1 + text_len, suffix, sizeof(suffix) - 1);
            ib_assert_fail(b, msg, total);

            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_STRING: {
            size_t count = node->as.string.count;
            AstStringPart *parts = node->as.string.parts;

            int all_literal = 1;
            for (size_t i = 0; i < count; i++) {
                if (parts[i].is_expr) { all_literal = 0; break; }
            }

            if (all_literal) {
                size_t total = 0;
                for (size_t i = 0; i < count; i++) total += parts[i].text_len;
                void *mem = asnix_arena_alloc(8 + total);
                *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)total;
                char *out = (char *)mem + ASNIX_STRING_OFF_BYTES;
                for (size_t i = 0; i < count; i++) {
                    memcpy(out, parts[i].text, parts[i].text_len);
                    out += parts[i].text_len;
                }
                Value v = { .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
                ib_push_const(b, (int64_t)v.raw);
                return;
            }

            size_t emit_count = 0;
            for (size_t i = 0; i < count; i++) {
                if (!parts[i].is_expr && parts[i].text_len == 0) continue;
                if (parts[i].is_expr) {
                    compile_expr(b, parts[i].expr, scope);
                } else {
                    Value v = asnix_make_string_value(parts[i].text, parts[i].text_len);
                    ib_push_const(b, (int64_t)v.raw);
                }
                emit_count++;
            }
            ib_str_concat(b, (int64_t)emit_count);
            return;
        }
        case AST_LIST: {
            for (size_t i = 0; i < node->as.list.count; i++) {
                if (try_cell_ref(b, node->as.list.elems[i], scope)) {
                } else if (is_thunk_free_literal(node->as.list.elems[i])) {
                    compile_expr(b, node->as.list.elems[i], scope);
                } else {
                    IrBuilder *elem_b = ib_new();
                    compile_expr(elem_b, node->as.list.elems[i], scope);
                    ib_ret(elem_b);
                    ib_mk_thunk(b, ib_finish(elem_b));
                }
            }
            ib_mk_list(b, (int64_t)node->as.list.count);
            return;
        }
        case AST_ATTRSET: {
            size_t n = node->as.attrset.count;
            uint32_t syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t order_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            int small_n = (n <= ASNIX_ATTRSET_SORT_INSERTION_MAX);
            uint32_t *syms = small_n ? syms_stack : malloc(n * sizeof(uint32_t));
            size_t *order = small_n ? order_stack : malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                syms[i] = asnix_intern(node->as.attrset.bindings[i].name, node->as.attrset.bindings[i].name_len);
                order[i] = i;
            }
            if (small_n) {
                for (size_t i = 1; i < n; i++) {
                    size_t j = i;
                    while (j > 0 && syms[order[j - 1]] > syms[order[j]]) {
                        size_t tmp = order[j - 1];
                        order[j - 1] = order[j];
                        order[j] = tmp;
                        j--;
                    }
                }
            } else {
                sort_attrset_bindings_large(syms, order, n);
            }
            uint32_t *sorted_syms = small_n ? sorted_syms_stack : malloc(n * sizeof(uint32_t));
            for (size_t i = 0; i < n; i++) sorted_syms[i] = syms[order[i]];

            if (!node->as.attrset.is_rec) {
                size_t hidden_count = 0;
                for (size_t i = 0; i < n; i++) if (node->as.attrset.bindings[i].is_hidden) hidden_count++;
                if (hidden_count == 0) {
                    for (size_t i = 0; i < n; i++) {
                        AstBinding *bind = &node->as.attrset.bindings[order[i]];
                        if (try_cell_ref(b, bind->value, scope)) {
                        } else if (is_thunk_free_literal(bind->value)) {
                            compile_expr(b, bind->value, scope);
                        } else {
                            emit_attrset_field_value(b, bind->value, scope);
                        }
                    }
                    ib_mk_attrs(b, (int64_t)n, sorted_syms);
                    for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, scope);
                        if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, scope)) {
                        } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                            compile_expr(b, node->as.attrset.dyn_bindings[i].value, scope);
                        } else {
                            IrBuilder *thunk_b = ib_new();
                            compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, scope);
                            ib_ret(thunk_b);
                            ib_mk_thunk(b, ib_finish(thunk_b));
                        }
                        ib_insert_dynamic_attr(b);
                    }
                    if (!small_n) { free(syms); free(order); free(sorted_syms); }
                    return;
                }
                const char *hnames_stack[ASNIX_COMPILE_SCRATCH_MAX];
                size_t hname_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
                const char **hnames = small_n ? hnames_stack : malloc((hidden_count ? hidden_count : 1) * sizeof(char *));
                size_t *hname_lens = small_n ? hname_lens_stack : malloc((hidden_count ? hidden_count : 1) * sizeof(size_t));
                size_t hi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (node->as.attrset.bindings[i].is_hidden) {
                        hnames[hi] = node->as.attrset.bindings[i].name;
                        hname_lens[hi] = node->as.attrset.bindings[i].name_len;
                        hi++;
                    }
                }
                ScopeFrame hidden_frame = { .up = scope, .names = hnames, .name_lens = hname_lens, .count = hidden_count };
                ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };
                ib_frame_alloc(b, (int64_t)hidden_count);
                hi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (!node->as.attrset.bindings[i].is_hidden) continue;
                    if (try_cell_ref(b, node->as.attrset.bindings[i].value, &skip)) {
                    } else if (is_thunk_free_literal(node->as.attrset.bindings[i].value)) {
                        compile_expr(b, node->as.attrset.bindings[i].value, &skip);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.bindings[i].value, &skip);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_frame_set(b, (int64_t)hi);
                    hi++;
                }
                size_t visible_count = n - hidden_count;
                uint32_t visible_sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
                uint32_t *visible_sorted_syms = small_n ? visible_sorted_syms_stack : malloc((visible_count ? visible_count : 1) * sizeof(uint32_t));
                size_t vi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (node->as.attrset.bindings[order[i]].is_hidden) continue;
                    if (try_cell_ref(b, node->as.attrset.bindings[order[i]].value, &hidden_frame)) {
                    } else if (is_thunk_free_literal(node->as.attrset.bindings[order[i]].value)) {
                        compile_expr(b, node->as.attrset.bindings[order[i]].value, &hidden_frame);
                    } else {
                        emit_attrset_field_value(b, node->as.attrset.bindings[order[i]].value, &hidden_frame);
                    }
                    visible_sorted_syms[vi++] = sorted_syms[i];
                }
                ib_mk_attrs(b, (int64_t)visible_count, visible_sorted_syms);
                for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                    compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, &hidden_frame);
                    if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, &hidden_frame)) {
                    } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].value, &hidden_frame);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, &hidden_frame);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_insert_dynamic_attr(b);
                }
                ib_frame_exit(b);
                if (!small_n) { free(hnames); free(hname_lens); free(visible_sorted_syms); }
            } else {
                AstBinding *bindings = node->as.attrset.bindings;
                const char *names_stack[ASNIX_COMPILE_SCRATCH_MAX];
                size_t name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
                const char **names = small_n ? names_stack : malloc(n * sizeof(char *));
                size_t *name_lens = small_n ? name_lens_stack : malloc(n * sizeof(size_t));
                for (size_t i = 0; i < n; i++) {
                    names[i] = bindings[i].name;
                    name_lens[i] = bindings[i].name_len;
                }
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
                ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

                int64_t overrides_slot = -1;
                for (size_t i = 0; i < n; i++) {
                    if (bindings[i].name_len == 11 && memcmp(bindings[i].name, "__overrides", 11) == 0) {
                        overrides_slot = (int64_t)i;
                        break;
                    }
                }
                frame.has_overrides = (overrides_slot >= 0);

                ib_frame_alloc(b, (int64_t)n);
                for (size_t i = 0; i < n; i++) {
                    ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                    if (try_cell_ref(b, bindings[i].value, value_scope)) {
                    } else if (is_thunk_free_literal(bindings[i].value)) {
                        compile_expr(b, bindings[i].value, value_scope);
                    } else {
                        emit_attrset_field_value(b, bindings[i].value, value_scope);
                    }
                    ib_frame_set(b, (int64_t)i);
                }
                if (overrides_slot >= 0) {
                    ib_rec_overrides_merge(b, (int64_t)n, syms, overrides_slot);
                } else {
                    size_t visible_count = 0;
                    for (size_t i = 0; i < n; i++) if (!bindings[order[i]].is_hidden) visible_count++;
                    uint32_t visible_sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
                    uint32_t *visible_sorted_syms = small_n ? visible_sorted_syms_stack : malloc((visible_count ? visible_count : 1) * sizeof(uint32_t));
                    size_t vi = 0;
                    for (size_t i = 0; i < n; i++) {
                        if (bindings[order[i]].is_hidden) continue;
                        ib_frame_get(b, (int64_t)order[i]);
                        visible_sorted_syms[vi++] = sorted_syms[i];
                    }
                    ib_mk_attrs(b, (int64_t)visible_count, visible_sorted_syms);
                    if (!small_n) free(visible_sorted_syms);
                }
                for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                    compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, &frame);
                    if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, &frame)) {
                    } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].value, &frame);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, &frame);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_insert_dynamic_attr(b);
                }
                ib_frame_exit(b);
                if (!small_n) { free(names); free(name_lens); }
            }
            if (!small_n) { free(syms); free(order); free(sorted_syms); }
            return;
        }
        case AST_SELECT: {
            if (!node->as.select.default_) {
                compile_expr(b, node->as.select.target, scope);
                if (node->as.select.attr_expr) {
                    compile_expr(b, node->as.select.attr_expr, scope);
                    ib_select_dynamic(b);
                    return;
                }
                uint32_t sym = asnix_intern(node->as.select.attr, node->as.select.attr_len);
                ib_select(b, sym);
                return;
            }
            Ast *default_ast = node->as.select.default_;
            size_t seg_cap = 4, seg_len = 0;
            Ast **segments = malloc(seg_cap * sizeof(Ast *));
            Ast *chain_node = node;
            for (;;) {
                if (seg_len == seg_cap) {
                    seg_cap *= 2;
                    segments = realloc(segments, seg_cap * sizeof(Ast *));
                }
                segments[seg_len++] = chain_node;
                Ast *next = chain_node->as.select.target;
                if (next->kind != AST_SELECT || next->as.select.default_ != default_ast) {
                    compile_expr(b, next, scope);
                    break;
                }
                chain_node = next;
            }
            size_t *try_sites = malloc(seg_len * sizeof(size_t));
            for (size_t i = 0; i < seg_len; i++) {
                Ast *seg = segments[seg_len - 1 - i];
                if (seg->as.select.attr_expr) {
                    compile_expr(b, seg->as.select.attr_expr, scope);
                    try_sites[i] = ib_select_dynamic_or(b);
                } else {
                    uint32_t sym = asnix_intern(seg->as.select.attr, seg->as.select.attr_len);
                    try_sites[i] = ib_select_or(b, sym);
                }
            }
            free(segments);
            size_t jmp_site = ib_jmp(b);
            size_t default_idx = ib_len(b);
            for (size_t i = 0; i < seg_len; i++) {
                ib_patch_delta(b, try_sites[i], default_idx);
            }
            free(try_sites);
            compile_expr(b, default_ast, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_WITH: {
            if (try_cell_ref(b, node->as.with1.attrs, scope)) {
            } else if (is_thunk_free_literal(node->as.with1.attrs)) {
                compile_expr(b, node->as.with1.attrs, scope);
            } else {
                IrBuilder *thunk_b = ib_new();
                compile_expr(thunk_b, node->as.with1.attrs, scope);
                ib_ret(thunk_b);
                ib_mk_thunk(b, ib_finish(thunk_b));
            }
            ib_with(b);
            ScopeFrame frame = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0, .is_with = 1 };
            compile_expr(b, node->as.with1.body, &frame);
            ib_with_exit(b);
            return;
        }
        case AST_PATH: {
            if (node->as.path.is_search) {
                Value name_v = asnix_make_string_value(node->as.path.text, node->as.path.len);
                ib_push_const(b, (int64_t)name_v.raw);
                ib_resolve_search_path(b);
                return;
            }
            if (node->as.path.interp_count > 0) {
                Value first = resolve_path_chunk(node->as.path.text, node->as.path.len, node->as.path.is_home);
                ib_push_const(b, (int64_t)first.raw);
                for (size_t i = 0; i < node->as.path.interp_count; i++) {
                    AstStringPart *part = &node->as.path.interp_parts[i];
                    if (part->is_expr) {
                        compile_expr(b, part->expr, scope);
                    } else {
                        Value sv = asnix_make_string_value(part->text, part->text_len);
                        ib_push_const(b, (int64_t)sv.raw);
                    }
                }
                ib_path_concat(b, (int64_t)(1 + node->as.path.interp_count));
                return;
            }
            char *abs = resolve_path_literal(node);
            size_t len = strlen(abs);
            Value v = asnix_make_path_value(abs, len);
            free(abs);
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
        case AST_CURPOS: {
            const char *file = g_compile_file_path ? g_compile_file_path : asnix_eval_top_level_file();
            if (!file) file = "";
            uint32_t syms[3];
            Value vals[3];
            syms[0] = asnix_intern("file", 4);
            vals[0] = asnix_make_string_value(file, strlen(file));
            syms[1] = asnix_intern("line", 4);
            vals[1] = asnix_make_int(node->as.curpos.line);
            syms[2] = asnix_intern("column", 6);
            vals[2] = asnix_make_int(node->as.curpos.column);
            Value v = asnix_bshared_make_attrs(syms, vals, 3);
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
    }
}

static void init_builtins_frame(ScopeFrame *f) {
    static const char *builtins_names[16] = {
        "builtins", "import", "map", "abort", "toString", "removeAttrs", "isNull", "throw", "derivation",
        "baseNameOf", "dirOf", "fromTOML", "placeholder", "scopedImport", "derivationStrict", "fetchGit" };
    static size_t builtins_name_lens[16] = { 8, 6, 3, 5, 8, 11, 6, 5, 10, 10, 5, 8, 11, 12, 16, 8 };
    f->up = NULL;
    f->names = builtins_names;
    f->name_lens = builtins_name_lens;
    f->count = 16;
    f->is_with = 0;
    f->has_overrides = 0;
}

IrProgram *asnix_compile(Ast *root, const char *base_dir, const char *file_path) {
    g_compile_base_dir = base_dir;
    g_compile_file_path = file_path;
    IrBuilder *b = ib_new();
    ScopeFrame builtins_frame;
    init_builtins_frame(&builtins_frame);
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}

IrProgram *asnix_compile_scoped(Ast *root, const char *base_dir, const char *file_path,
                                 const char **extra_names, size_t *extra_lens, size_t extra_count) {
    g_compile_base_dir = base_dir;
    g_compile_file_path = file_path;
    IrBuilder *b = ib_new();
    ScopeFrame builtins_frame;
    init_builtins_frame(&builtins_frame);
    ScopeFrame extra_frame = {
        .up = &builtins_frame, .names = extra_names, .name_lens = extra_lens, .count = extra_count, .is_with = 0 };
    compile_expr(b, root, &extra_frame);
    ib_halt(b);
    return ib_finish(b);
}

