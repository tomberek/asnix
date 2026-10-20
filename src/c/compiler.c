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

/* One compile-time scope level == one runtime Env frame: `count` names,
 * each living at its own slot (De Bruijn depth = how many ScopeFrame.up
 * links away; slot = index within `names`/`name_lens`). AST_LAMBDA and the
 * root `builtins` frame just happen to use count==1 for the plain-
 * identifier case; AST_LET, `rec {}`, and a lambda-pattern's destructured
 * fields use count==(number of names) so every binding in one block
 * shares a single frame -- the structural piece that makes them mutually
 * visible (see compiler.c's AST_LET/AST_ATTRSET/AST_LAMBDA cases and
 * ir.h's OP_FRAME_* ops).
 *
 * `with` (see the AST_WITH case below) also introduces a ScopeFrame -- one
 * new runtime Env frame per `with`, exactly like let/lambda -- but with
 * `count == 0` (no statically-known names at all) and `is_with` set: it
 * never matches a direct lexical lookup, yet still has to count as one
 * more hop for every *other* name's depth, since depth is "how many
 * runtime Env frames up" and a with-frame really is one. `is_with` is what
 * lets AST_VAR's walk notice it and fall back to a bounded chain of
 * runtime lookups against each enclosing with-value, innermost first --
 * see the AST_VAR case. `count == 0` alone is NOT enough to mean "with
 * frame": the dummy `skip` level AST_LET builds for `inherit`'s outer-
 * scope resolution (see that case) is also zero-count but must NOT join
 * the with-fallback chain, hence the separate flag. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char **names;
    size_t *name_lens;
    size_t count;
    int is_with;
} ScopeFrame;

/* Bounds how many enclosing `with`s a single unresolved-name fallback
 * chain can walk through -- real Nix has no such limit, but nesting this
 * deep never happens in practice and a fixed-size stack array keeps the
 * AST_VAR case simple (same spirit as other deliberate, documented scope
 * cuts in this project, e.g. M6's non-recursive `let`). */
#define ASNIX_MAX_WITH_CHAIN 64

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return a != NULL && alen == blen && memcmp(a, b, alen) == 0;
}

/* Insertion sort by symbol id -- every call site sorts a source-level
 * formal/binding count, always tiny. */
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
            int64_t with_depths[ASNIX_MAX_WITH_CHAIN];
            size_t with_count = 0;
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                /* A lexical binding always wins over any enclosing `with`,
                 * even one introduced further out than this match (real
                 * Nix semantics: `with` is a fallback, never a shadow) --
                 * returning here before ever looking at with_depths is
                 * exactly that. */
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
            /* No lexical match anywhere -- fall back to a runtime search
             * over each enclosing `with`'s attrset, innermost first,
             * stopping at the first one that has the key (whether it
             * does is a runtime fact; the *chain itself*, built here, is
             * entirely compile-time since the with-nesting is static).
             * Each level: OP_VAR_REF reads+forces that with-frame's slot 0
             * (an ordinary lexical lookup -- the with-frame's depth is
             * already known), OP_WITH_TRY searches it and either falls
             * through with a match or jumps to the next level, and the
             * OP_JMP right after only runs on a hit, skipping straight to
             * `end` past every remaining level. If no level has the key,
             * execution falls into OP_WITH_MISS. */
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
            /* Real Nix `let ... in` is implicitly `letrec`: every binding
             * is visible to itself and to every sibling binding in the
             * same block, regardless of source order. OP_FRAME_ALLOC
             * allocates this let's Env frame and installs it as ENV
             * *before* any binding's thunk is built, so each OP_MK_THUNK
             * (still lazy, same as before) naturally captures this frame
             * as its scope -- self/sibling references just resolve
             * through the frame like any other variable, once forced.
             * `inherit`ed bindings are the one exception: their value
             * must resolve against the scope *enclosing* this `let`, not
             * this new frame (see ast.h). It's NOT enough to just compile
             * them against `scope` directly, though: by the time any
             * binding's thunk is built (including the inherited one),
             * OP_FRAME_ALLOC has already switched the *runtime* ENV to
             * this new frame, so whatever OP_MK_THUNK captures sits one
             * real Env level below `scope`'s corresponding env. `skip`
             * is a dummy, name-less scope level (matches nothing, so
             * every lookup falls through it to `scope`) wrapped around
             * `scope` for exactly this purpose: it makes the compile-time
             * depth count include that one extra real frame, so it lines
             * back up with where the name actually lives at runtime. */
            size_t n = node->as.let1.count;
            AstBinding *bindings = node->as.let1.bindings;
            const char **names = malloc(n * sizeof(char *));
            size_t *name_lens = malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                names[i] = bindings[i].name;
                name_lens[i] = bindings[i].name_len;
            }
            ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
            ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

            ib_frame_alloc(b, (int64_t)n);
            for (size_t i = 0; i < n; i++) {
                ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                IrBuilder *thunk_b = ib_new();
                compile_expr(thunk_b, bindings[i].value, value_scope);
                ib_ret(thunk_b);
                ib_mk_thunk(b, ib_finish(thunk_b));
                ib_frame_set(b, (int64_t)i);
            }
            compile_expr(b, node->as.let1.body, &frame);
            ib_frame_exit(b);
            free(names);
            free(name_lens);
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
            if (!node->as.lambda.is_pattern) {
                IrBuilder *body_b = ib_new();
                const char *names[1] = { node->as.lambda.param };
                size_t name_lens[1] = { node->as.lambda.param_len };
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = 1 };
                compile_expr(body_b, node->as.lambda.body, &frame);
                ib_ret(body_b);
                IrProgram *body_prog = ib_finish(body_b);
                ib_lambda(b, 1, body_prog);
                return;
            }

            /* Lambda pattern (`{a, b ? d, ...}: body` / with an optional
             * `@name` alias either side): still arity-1 at the OP_CALL ABI
             * level -- op_call.S/Closure don't change at all. The call's
             * one real attrset argument lands in a throwaway "arg frame"
             * (depth 1 from the body's point of view), and a *second*
             * frame (depth 0) holds one thunk per declared field.
             *
             * That second frame is allocated via OP_FRAME_NEW *before* any
             * field's thunk is constructed (OP_FRAME_BIND_SLOT fills each
             * slot only after its thunk already exists) so every field's
             * thunk captures this frame, already-current, as its own
             * scope -- giving fields letrec-style mutual/forward
             * visibility, confirmed against real Nix
             * (`{a ? b+1, b ? 5}: a` applied to `{}` sees `b` from `a`'s
             * default even though `b` is declared after `a`). This is the
             * same "allocate the frame before constructing the thunks" fix
             * docs/architecture.md's M6 notes flag as what plain `let`
             * would need to become recursive -- applied narrowly to this
             * one synthetic frame, not to OP_LET/general `let` or attrset
             * bindings, which keep their documented non-recursive
             * limitation unchanged. */
            size_t nf = node->as.lambda.formals_count;
            IrBuilder *body_b = ib_new();

            const char *arg_names[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name : "" };
            size_t arg_name_lens[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name_len : 0 };
            ScopeFrame argframe = { .up = scope, .names = arg_names, .name_lens = arg_name_lens, .count = 1 };

            /* -- Eager validation (OP_CHECK_FORMALS), matching real Nix's
             * timing exactly: a type mismatch, a missing required field,
             * or (without `...`) an extra field all abort immediately on
             * application, even if the body never reads the offending
             * field (confirmed against nix-instantiate). -- */
            uint32_t *all_syms = malloc((nf ? nf : 1) * sizeof(uint32_t));
            size_t required_count = 0;
            for (size_t i = 0; i < nf; i++) {
                all_syms[i] = asnix_intern(node->as.lambda.formals[i].name, node->as.lambda.formals[i].name_len);
                if (!node->as.lambda.formals[i].default_) required_count++;
            }
            uint32_t *required_syms = malloc((required_count ? required_count : 1) * sizeof(uint32_t));
            {
                size_t k = 0;
                for (size_t i = 0; i < nf; i++) {
                    if (!node->as.lambda.formals[i].default_) required_syms[k++] = all_syms[i];
                }
            }
            sort_syms(required_syms, required_count);
            uint32_t *all_syms_sorted = NULL;
            int64_t total_count = -1; /* `...`: no "unexpected argument" check at all */
            if (!node->as.lambda.has_ellipsis) {
                all_syms_sorted = malloc((nf ? nf : 1) * sizeof(uint32_t));
                memcpy(all_syms_sorted, all_syms, nf * sizeof(uint32_t));
                sort_syms(all_syms_sorted, nf);
                total_count = (int64_t)nf;
            }

            ib_var_ref(body_b, 0, 0); /* the raw attrset arg: argframe is depth 0 until OP_FRAME_NEW below */
            ib_check_formals(body_b, (int64_t)required_count, required_syms, total_count, all_syms_sorted);
            free(all_syms);
            free(required_syms);
            free(all_syms_sorted);

            /* -- The letrec-capable field frame. -- */
            ib_frame_alloc(body_b, (int64_t)nf);

            const char **field_names = malloc((nf ? nf : 1) * sizeof(char *));
            size_t *field_name_lens = malloc((nf ? nf : 1) * sizeof(size_t));
            for (size_t i = 0; i < nf; i++) {
                field_names[i] = node->as.lambda.formals[i].name;
                field_name_lens[i] = node->as.lambda.formals[i].name_len;
            }
            ScopeFrame fieldframe = { .up = &argframe, .names = field_names, .name_lens = field_name_lens, .count = nf };

            for (size_t i = 0; i < nf; i++) {
                /* Each field's thunk body resolves names against
                 * `fieldframe` (siblings, incl. itself and later fields --
                 * letrec) with `argframe` one level up (the raw attrset,
                 * and -- if given -- its `@` alias). Built from synthetic
                 * AST nodes so it goes through the exact same compile_expr
                 * paths as hand-written source would. */
                Ast *arg_var = asnix_ast_new(AST_VAR);
                arg_var->as.var.name = arg_names[0];
                arg_var->as.var.name_len = arg_name_lens[0];

                Ast *select_node = asnix_ast_new(AST_SELECT);
                select_node->as.select.target = arg_var;
                select_node->as.select.attr = node->as.lambda.formals[i].name;
                select_node->as.select.attr_len = node->as.lambda.formals[i].name_len;

                Ast *field_body;
                if (node->as.lambda.formals[i].default_ == NULL) {
                    field_body = select_node; /* required: plain `arg.field` (aborts if missing, but
                                                * OP_CHECK_FORMALS above already guaranteed it's present) */
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

                IrBuilder *thunk_b = ib_new();
                compile_expr(thunk_b, field_body, &fieldframe);
                ib_ret(thunk_b);
                ib_mk_thunk(body_b, ib_finish(thunk_b));
                ib_frame_set(body_b, (int64_t)i);
            }

            compile_expr(body_b, node->as.lambda.body, &fieldframe);
            ib_ret(body_b);
            IrProgram *body_prog = ib_finish(body_b);
            free(field_names);
            free(field_name_lens);
            ib_lambda(b, 1, body_prog);
            return;
        }
        case AST_HAS_ATTR: {
            compile_expr(b, node->as.has_attr.target, scope);
            uint32_t sym = asnix_intern(node->as.has_attr.attr, node->as.has_attr.attr_len);
            ib_has_attr(b, sym);
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
                case 'e': ib_eq(b); return;
                case 'n': ib_neq(b); return;
                case 'l': ib_lt(b); return;
                case 'g': ib_gt(b); return;
                case 'L': ib_le(b); return;
                case 'G': ib_ge(b); return;
                case 'u': ib_update(b); return;
                case 'c': ib_concat(b); return;
            }
            return;
        case AST_AND: {
            /* Short-circuit, compiled branch-shaped like `if lhs then rhs
             * else false` (NOT as an eager binop) -- see ir.h's OP_EQ
             * comment: the untaken side's IR is simply never reached by
             * the jmp-chain dispatch loop, so it's provably never
             * evaluated (verified against real nix-instantiate with
             * `false && (assert false; true)`, see tests/e2e). */
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
            /* `if lhs then true else rhs` -- same short-circuit rationale as AST_AND. */
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
            /* `if cond then body else <fatal, naming the failed cond>` --
             * same branch shape as AST_IF, reusing OP_IF/OP_JMP wholesale;
             * only the "else" side differs (OP_ASSERT_FAIL instead of
             * compiled code), see op_assert.S. */
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
                /* No interpolation at all (the common case, including every
                 * plain "..."/''...'' literal) -- fold into one compile-time
                 * constant, exactly like pre-interpolation M5 did. */
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

            /* At least one interpolated part: push each literal chunk (as
             * its own string constant) and each interpolated expression's
             * raw result, then let OP_STR_CONCAT (runtime) coerce the
             * non-string ones and concatenate everything in source order --
             * see docs/architecture.md's strings scope notes. */
            for (size_t i = 0; i < count; i++) {
                if (parts[i].is_expr) {
                    compile_expr(b, parts[i].expr, scope);
                } else {
                    Value v = asnix_make_string_value(parts[i].text, parts[i].text_len);
                    ib_push_const(b, (int64_t)v.raw);
                }
            }
            ib_str_concat(b, (int64_t)count);
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
            for (size_t i = 0; i < n; i++) sorted_syms[i] = syms[order[i]];

            if (!node->as.attrset.is_rec) {
                /* Plain `{ ... }`: lazy (each binding's value is wrapped
                 * in a thunk captured over the *enclosing* scope), but
                 * deliberately NOT recursive -- a binding can't reference
                 * `self` or a sibling key, matching real Nix (only
                 * `rec { ... }` gets that). `inherit` here is just an
                 * ordinary binding whose value happens to be an
                 * AST_VAR/AST_SELECT that also resolves against `scope`,
                 * so it needs no special handling. */
                for (size_t i = 0; i < n; i++) {
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, node->as.attrset.bindings[order[i]].value, scope);
                    ib_ret(thunk_b);
                    ib_mk_thunk(b, ib_finish(thunk_b));
                }
                ib_mk_attrs(b, (int64_t)n, sorted_syms);
            } else {
                /* `rec { ... }`: same frame-first trick as AST_LET, keyed
                 * by original (unsorted) binding index so the ScopeFrame
                 * slot a binding's thunk-body references lines up with
                 * the OP_FRAME_SET that filed it. Once every binding is
                 * filed, OP_VAR_REF(0, slot) reads each value straight
                 * back off the frame in sorted order -- reusing the exact
                 * same OP_MK_ATTRS the non-rec path uses. */
                AstBinding *bindings = node->as.attrset.bindings;
                const char **names = malloc(n * sizeof(char *));
                size_t *name_lens = malloc(n * sizeof(size_t));
                for (size_t i = 0; i < n; i++) {
                    names[i] = bindings[i].name;
                    name_lens[i] = bindings[i].name_len;
                }
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
                /* `skip`: see the matching comment in AST_LET's case --
                 * same reason a bare `inherit x;` here can't just compile
                 * against `scope` directly (OP_FRAME_ALLOC already
                 * switched the runtime ENV by the time any binding's
                 * thunk is built). */
                ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

                ib_frame_alloc(b, (int64_t)n);
                for (size_t i = 0; i < n; i++) {
                    ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, bindings[i].value, value_scope);
                    ib_ret(thunk_b);
                    ib_mk_thunk(b, ib_finish(thunk_b));
                    ib_frame_set(b, (int64_t)i);
                }
                for (size_t i = 0; i < n; i++) ib_var_ref(b, 0, (int64_t)order[i]);
                ib_mk_attrs(b, (int64_t)n, sorted_syms);
                ib_frame_exit(b);
                free(names);
                free(name_lens);
            }
            free(syms);
            free(order);
            free(sorted_syms);
            return;
        }
        case AST_SELECT: {
            compile_expr(b, node->as.select.target, scope);
            if (node->as.select.attr_expr) {
                /* Dynamic `.${expr}` -- the attribute name isn't known
                 * until runtime, so there's no sym to intern here; the
                 * name expr's *value* (a string) is pushed and
                 * OP_SELECT_DYNAMIC (src/asm/ops/op_select_dynamic.S)
                 * interns/looks it up at runtime (see
                 * asnix_bshared_select_dynamic, builtins_shared.c). `or`
                 * combined with a dynamic name is rejected at parse time
                 * (parser.c), so attr_expr and default_ are never both
                 * set here. */
                compile_expr(b, node->as.select.attr_expr, scope);
                ib_select_dynamic(b);
                return;
            }
            uint32_t sym = asnix_intern(node->as.select.attr, node->as.select.attr_len);
            if (!node->as.select.default_) {
                ib_select(b, sym);
                return;
            }
            /* `expr.attr or default`: OP_SELECT_OR falls through in-line
             * on a hit (value already on VALSTACK); on a miss it jumps
             * (by the delta patched below) straight to the default's
             * code. The OP_JMP right after is only reached on the hit
             * path, skipping over the default's code to the shared end --
             * same two-site patch-after-the-fact shape AST_IF already
             * uses for its then/else branches. */
            size_t try_site = ib_select_or(b, sym);
            size_t jmp_site = ib_jmp(b);
            size_t default_idx = ib_len(b);
            ib_patch_delta(b, try_site, default_idx);
            compile_expr(b, node->as.select.default_, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_WITH: {
            /* Lazy, same as `let`/attrset bindings (M6): the with-expr is
             * compiled as its own thunk body captured over the *outer*
             * scope (it can't see names `with` itself introduces), so a
             * `with` whose value is never actually consulted (every name
             * in `body` resolves lexically) never forces it -- matches
             * real Nix (verified: `with (builtins.trace "x" {}); 5` never
             * prints the trace). */
            IrBuilder *thunk_b = ib_new();
            compile_expr(thunk_b, node->as.with1.attrs, scope);
            ib_ret(thunk_b);
            ib_mk_thunk(b, ib_finish(thunk_b));
            ib_with(b);
            ScopeFrame frame = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0, .is_with = 1 };
            compile_expr(b, node->as.with1.body, &frame);
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
    /* The root env (constructed by eval_file.c) has eight slots: the
     * `builtins` attrset, plus `import`/`map`/`abort`/`toString`/
     * `removeAttrs`/`isNull`/`throw` bare -- seven of the small set of
     * names real Nix exposes as bare globals in addition to `builtins.X`
     * (see builtins.h's asnix_make_import_primop comment). This scope
     * frame makes `builtins.toString 5` and bare `import ./x`/`map f xs`/
     * `abort "msg"`/`toString 5`/`removeAttrs set ["a"]`/`isNull x`/
     * `throw "msg"` all resolve like any other variable reference. */
    static const char *builtins_names[8] = {
        "builtins", "import", "map", "abort", "toString", "removeAttrs", "isNull", "throw" };
    static size_t builtins_name_lens[8] = { 8, 6, 3, 5, 8, 11, 6, 5 };
    ScopeFrame builtins_frame = {
        .up = NULL, .names = builtins_names, .name_lens = builtins_name_lens, .count = 8 };
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}
