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
