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

/* Every scope-introducing AST node (AST_LET, plain-identifier AST_LAMBDA)
 * binds exactly one name (see ast.h), but a lambda-pattern's field frame
 * (compiler.c's AST_LAMBDA case, below) binds several names at once, all
 * in the *same* Env frame -- so a ScopeFrame now holds a whole frame's
 * worth of (name, slot) pairs rather than hard-coding slot 0, and "depth"
 * is "how many ScopeFrame links up", matching one physical Env frame per
 * link exactly as before. A real attrset/`with`-capable resolver (M5+)
 * will need a fuller rewrite; this is a deliberate M4-era shortcut,
 * widened just enough for lambda-patterns. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char *const *names;
    const size_t *name_lens;
    size_t nslots;
} ScopeFrame;

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
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
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                for (size_t slot = 0; slot < f->nslots; slot++) {
                    if (name_eq(f->names[slot], f->name_lens[slot], node->as.var.name, node->as.var.name_len)) {
                        ib_var_ref(b, depth, (int64_t)slot);
                        return;
                    }
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
            const char *let_names[1] = { node->as.let1.name };
            size_t let_name_lens[1] = { node->as.let1.name_len };
            ScopeFrame frame = { .up = scope, .names = let_names, .name_lens = let_name_lens, .nslots = 1 };
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
            if (!node->as.lambda.is_pattern) {
                IrBuilder *body_b = ib_new();
                const char *names[1] = { node->as.lambda.param };
                size_t name_lens[1] = { node->as.lambda.param_len };
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .nslots = 1 };
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
            ScopeFrame argframe = { .up = scope, .names = arg_names, .name_lens = arg_name_lens, .nslots = 1 };

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
            ib_frame_new(body_b, (int64_t)nf);

            const char **field_names = malloc((nf ? nf : 1) * sizeof(char *));
            size_t *field_name_lens = malloc((nf ? nf : 1) * sizeof(size_t));
            for (size_t i = 0; i < nf; i++) {
                field_names[i] = node->as.lambda.formals[i].name;
                field_name_lens[i] = node->as.lambda.formals[i].name_len;
            }
            ScopeFrame fieldframe = { .up = &argframe, .names = field_names, .name_lens = field_name_lens, .nslots = nf };

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
                ib_frame_bind_slot(body_b, (int64_t)i);
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
    }
}

IrProgram *asnix_compile(Ast *root) {
    IrBuilder *b = ib_new();
    /* The root env (constructed by main.c) has exactly one slot: the
     * `builtins` attrset. This scope frame makes `builtins.toString 5`
     * etc. resolve like any other variable reference. */
    const char *root_names[1] = { "builtins" };
    size_t root_name_lens[1] = { 8 };
    ScopeFrame builtins_frame = { .up = NULL, .names = root_names, .name_lens = root_name_lens, .nslots = 1 };
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}
