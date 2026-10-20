#include "compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "symtab.h"

/* Every scope-introducing AST node (AST_LET, AST_LAMBDA) binds exactly one
 * name at slot 0 (see ast.h), so resolution is just "how many scope links
 * up is the matching name" -- a single combined resolve+emit walk, no
 * separate annotation pass needed at this scope.
 *
 * `with` (see the AST_WITH case below) introduces a ScopeFrame too -- one
 * new runtime Env frame per `with`, exactly like let/lambda -- but with
 * `name == NULL`: it never matches a direct lexical lookup (a `with` binds
 * no statically-known names), yet still has to count as one more hop for
 * every *other* name's depth, since depth is "how many runtime Env frames
 * up" and a with-frame really is one. A `with`-marker frame's only other
 * job is to be noticed while walking past it, so an unresolved name can
 * fall back to a bounded chain of runtime lookups against each enclosing
 * with-value, innermost first -- see the AST_VAR case. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char *name;   /* NULL marks a `with`-introduced frame */
    size_t name_len;
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
                if (name_eq(f->name, f->name_len, node->as.var.name, node->as.var.name_len)) {
                    /* A lexical binding always wins over any enclosing
                     * `with`, even one introduced further out than this
                     * match (real Nix semantics: `with` is a fallback,
                     * never a shadow) -- returning here before ever
                     * looking at with_depths is exactly that. */
                    ib_var_ref(b, depth, 0);
                    return;
                }
                if (f->name == NULL) {
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
            ScopeFrame frame = { .up = scope, .name = NULL, .name_len = 0 };
            compile_expr(b, node->as.with1.body, &frame);
            return;
        }
    }
}

IrProgram *asnix_compile(Ast *root) {
    IrBuilder *b = ib_new();
    /* The root env (constructed by main.c) has exactly one slot: the
     * `builtins` attrset. This scope frame makes `builtins.toString 5`
     * etc. resolve like any other variable reference. */
    ScopeFrame builtins_frame = { .up = NULL, .name = "builtins", .name_len = 8 };
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}
