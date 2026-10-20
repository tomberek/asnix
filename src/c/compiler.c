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
#include "value_util.h"

/* One compile-time scope level == one runtime Env frame: `count` names,
 * each living at its own slot (De Bruijn depth = how many ScopeFrame.up
 * links away; slot = index within `names`/`name_lens`). AST_LAMBDA and the
 * root `builtins` frame just happen to use count==1; AST_LET and `rec {}`
 * use count==(number of bindings) so every binding in one block shares a
 * single frame -- the structural piece that makes them mutually visible
 * (see compiler.c's AST_LET/AST_ATTRSET cases and ir.h's OP_FRAME_* ops).
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
            ScopeFrame frame = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0, .is_with = 1 };
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
    static const char *builtins_names[1] = { "builtins" };
    static size_t builtins_name_lens[1] = { 8 };
    ScopeFrame builtins_frame = {
        .up = NULL, .names = builtins_names, .name_lens = builtins_name_lens, .count = 1 };
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}
