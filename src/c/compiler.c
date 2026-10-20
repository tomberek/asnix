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
#include "symtab.h"
#include "value_util.h"

/* AST_PATH resolution (import-paths). `base_dir` is constant for one whole
 * asnix_compile() call (every path literal in a given file resolves
 * against that *file's* directory, not wherever the thunk/lambda body
 * containing it happens to run later) -- threading it through every
 * compile_expr recursive call as an explicit parameter would touch every
 * call site for no benefit, so it's stashed here instead. Safe as a
 * single global rather than a stack: asnix_compile() always runs to
 * completion (there is no `call asnix_compile` while another
 * asnix_compile() frame is still on the C stack) before asnix_eval() ever
 * starts running the IR that might later trigger `import` (which runs a
 * *fresh, separate* asnix_eval_file() call, well after this one
 * returned) -- see src/c/eval_file.c. */
static const char *g_compile_base_dir = NULL;

/* Lexically canonicalizes an absolute path in place: collapses `.` and
 * `..` components and doubled slashes, same as real Nix's `canonPath`
 * (verified against `nix-instantiate --eval 'builtins.toString
 * ./sub/../foo.nix'`), but -- unlike `canonPath` -- never touches the
 * filesystem (no symlink resolution): cheap, and sufficient for every
 * case this project's fixtures exercise. `in` must start with '/'. */
static char *canon_path(const char *in) {
    size_t len = strlen(in);
    char *out = malloc(len + 2);
    char **comps = malloc((len + 1) * sizeof(char *));
    size_t ncomps = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && in[i] == '/') i++;
        size_t start = i;
        while (i < len && in[i] != '/') i++;
        size_t clen = i - start;
        if (clen == 0) continue;
        if (clen == 1 && in[start] == '.') continue; /* "." -- drop */
        if (clen == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (ncomps > 0) ncomps--; /* ".." -- pop (no-op at root, like real Nix) */
            continue;
        }
        char *c = malloc(clen + 1);
        memcpy(c, in + start, clen);
        c[clen] = '\0';
        comps[ncomps++] = c;
    }

    size_t pos = 0;
    for (size_t k = 0; k < ncomps; k++) {
        out[pos++] = '/';
        size_t clen = strlen(comps[k]);
        memcpy(out + pos, comps[k], clen);
        pos += clen;
        free(comps[k]);
    }
    free(comps);
    if (pos == 0) out[pos++] = '/';
    out[pos] = '\0';
    return out;
}

/* Resolves a NIX_PATH search-path entry name (the text inside `<...>`,
 * e.g. "nixpkgs" or "nixpkgs/lib") the same way real Nix's `findFile`
 * does: NIX_PATH is a colon-separated list of either `name=dir` (matched
 * by exact leading-component name) or plain `dir` entries (searched by
 * checking whether `dir/<name>` exists); first match wins. Aborts with a
 * message matching nix-instantiate's wording if nothing matches or
 * NIX_PATH isn't set. */
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
                    matched = 1; /* plain dir entries are always probed below */
                }
                if (matched) {
                    size_t dir_len = strlen(dir);
                    size_t cand_len = dir_len + 1 + name_len;
                    char *cand = malloc(cand_len + 1);
                    memcpy(cand, dir, dir_len);
                    cand[dir_len] = '/';
                    memcpy(cand + dir_len + 1, name, name_len);
                    cand[cand_len] = '\0';
                    if (eq) return cand; /* exact `name=dir` match: trust it, no stat needed */
                    struct stat st;
                    if (stat(cand, &st) == 0) return cand;
                    free(cand);
                }
            }
            entry = colon ? colon + 1 : NULL;
        }
        free(copy);
    }

    fprintf(stderr, "asnix: file '%.*s' was not found in the Nix search path (set $NIX_PATH)\n",
            (int)name_len, name);
    exit(1);
}

/* Resolves one AST_PATH node's raw lexeme to an absolute, canonicalized
 * path string (malloc'd, NUL-terminated). */
static char *resolve_path_literal(Ast *node) {
    const char *text = node->as.path.text;
    size_t len = node->as.path.len;

    if (node->as.path.is_search) return canon_path(resolve_search_path(text, len));

    if (len > 0 && text[len - 1] == '/') {
        fprintf(stderr, "asnix: path has a trailing slash\n");
        exit(1);
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

/* Every scope-introducing AST node (AST_LET, AST_LAMBDA) binds exactly one
 * name at slot 0 (see ast.h), so resolution is just "how many scope links
 * up is the matching name" -- a single combined resolve+emit walk, no
 * separate annotation pass needed at this scope. A real attrset/`with`-
 * capable resolver (M5+) will need one; this is a deliberate M4 shortcut. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char *name;
    size_t name_len;
} ScopeFrame;

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    switch (node->kind) {
        case AST_INT:
            ib_push_int(b, node->as.int_val);
            return;
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
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                if (name_eq(f->name, f->name_len, node->as.var.name, node->as.var.name_len)) {
                    ib_var_ref(b, depth, 0);
                    return;
                }
            }
            fprintf(stderr, "asnix: undefined variable '%.*s'\n", (int)node->as.var.name_len,
                    node->as.var.name);
            exit(1);
        }
        case AST_LET: {
            /* M6: the bound value is lazy -- compiled as its own body
             * (like a lambda's), wrapped in a thunk captured over the
             * *current* (outer) scope, not evaluated here. Depths inside
             * that body still resolve correctly against `scope`: at
             * runtime the thunk's captured env is exactly whatever env is
             * active right now, so this is identical to compiling the
             * same expression inline, just deferred.
             *
             * Note (scope limitation): this makes bindings lazy but NOT
             * recursive -- a binding cannot reference itself or sibling
             * bindings in the same `let` (unlike real Nix, where `let` is
             * implicitly `letrec`). Fixing that needs OP_LET restructured
             * to allocate the frame *before* constructing the thunk(s), so
             * they can capture it as their own scope -- not done here; see
             * docs/architecture.md's M6 scope notes. */
            IrBuilder *thunk_b = ib_new();
            compile_expr(thunk_b, node->as.let1.value, scope);
            ib_ret(thunk_b);
            ib_mk_thunk(b, ib_finish(thunk_b));
            ib_let(b, 1);
            ScopeFrame frame = { .up = scope, .name = node->as.let1.name, .name_len = node->as.let1.name_len };
            compile_expr(b, node->as.let1.body, &frame);
            return;
        }
        case AST_IF: {
            compile_expr(b, node->as.if1.cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.if1.then_, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, node->as.if1.else_, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_LAMBDA: {
            IrBuilder *body_b = ib_new();
            ScopeFrame frame = { .up = scope, .name = node->as.lambda.param, .name_len = node->as.lambda.param_len };
            compile_expr(body_b, node->as.lambda.body, &frame);
            ib_ret(body_b);
            IrProgram *body_prog = ib_finish(body_b);
            ib_lambda(b, 1, body_prog);
            return;
        }
        case AST_APP:
            compile_expr(b, node->as.app.fn, scope);
            compile_expr(b, node->as.app.arg, scope);
            ib_call(b, 1);
            return;
        case AST_BINOP:
            compile_expr(b, node->as.binop.lhs, scope);
            compile_expr(b, node->as.binop.rhs, scope);
            switch (node->as.binop.op) {
                case '+': ib_add(b); return;
                case '-': ib_sub(b); return;
                case '*': ib_mul(b); return;
            }
            return;
        case AST_STRING: {
            size_t len = node->as.string.len;
            void *mem = asnix_arena_alloc(8 + len);
            *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)len;
            memcpy((char *)mem + ASNIX_STRING_OFF_BYTES, node->as.string.text, len);
            Value v = { .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
        case AST_LIST: {
            for (size_t i = 0; i < node->as.list.count; i++) compile_expr(b, node->as.list.elems[i], scope);
            ib_mk_list(b, (int64_t)node->as.list.count);
            return;
        }
        case AST_ATTRSET: {
            size_t n = node->as.attrset.count;
            uint32_t *syms = malloc(n * sizeof(uint32_t));
            size_t *order = malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                syms[i] = asnix_intern(node->as.attrset.bindings[i].name, node->as.attrset.bindings[i].name_len);
                order[i] = i;
            }
            /* Insertion sort `order` by symbol id -- n is a source-level
             * binding count, always tiny. */
            for (size_t i = 1; i < n; i++) {
                size_t j = i;
                while (j > 0 && syms[order[j - 1]] > syms[order[j]]) {
                    size_t tmp = order[j - 1];
                    order[j - 1] = order[j];
                    order[j] = tmp;
                    j--;
                }
            }
            uint32_t *sorted_syms = malloc(n * sizeof(uint32_t));
            for (size_t i = 0; i < n; i++) {
                sorted_syms[i] = syms[order[i]];
                /* M6: lazy, same rationale as AST_LET above -- also not
                 * recursive, so an attrset binding can't reference `self`
                 * or sibling keys (no `rec { ... }` support yet). */
                IrBuilder *thunk_b = ib_new();
                compile_expr(thunk_b, node->as.attrset.bindings[order[i]].value, scope);
                ib_ret(thunk_b);
                ib_mk_thunk(b, ib_finish(thunk_b));
            }
            ib_mk_attrs(b, (int64_t)n, sorted_syms);
            free(syms);
            free(order);
            free(sorted_syms);
            return;
        }
        case AST_SELECT: {
            compile_expr(b, node->as.select.target, scope);
            uint32_t sym = asnix_intern(node->as.select.attr, node->as.select.attr_len);
            ib_select(b, sym);
            return;
        }
        case AST_PATH: {
            /* Resolved once, here, against g_compile_base_dir -- not at
             * eval time -- matching real Nix (a path literal's absolute
             * form is fixed by where it's *written*, verified against
             * nix-instantiate; see this file's top-of-file comment and
             * resolve_path_literal()). */
            char *abs = resolve_path_literal(node);
            size_t len = strlen(abs);
            Value v = asnix_make_path_value(abs, len);
            free(abs);
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
    }
}

IrProgram *asnix_compile(Ast *root, const char *base_dir) {
    g_compile_base_dir = base_dir;
    IrBuilder *b = ib_new();
    /* The root env (constructed by main.c) has exactly one slot: the
     * `builtins` attrset. This scope frame makes `builtins.toString 5`
     * etc. resolve like any other variable reference. */
    ScopeFrame builtins_frame = { .up = NULL, .name = "builtins", .name_len = 8 };
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}
