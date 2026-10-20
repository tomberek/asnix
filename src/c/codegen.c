#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Every byte sequence emitted below was cross-checked against `as`+`objdump`
 * for the equivalent hand-written instruction before being hard-coded here
 * (see the session's enc_check*.S scratch files) -- this file hand-encodes
 * x86-64 directly rather than extracting relocations from compiled .o
 * files (the fuller technique sketched in docs/architecture.md's M4.5
 * section), which is the scope tradeoff that kept this milestone tractable;
 * see codegen.h for exactly what's covered.
 */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} CodeBuf;

static void emit_bytes(CodeBuf *cb, const uint8_t *bytes, size_t n) {
    if (cb->len + n > cb->cap) {
        cb->cap = cb->cap ? cb->cap * 2 : 256;
        while (cb->len + n > cb->cap) cb->cap *= 2;
        cb->buf = realloc(cb->buf, cb->cap);
    }
    memcpy(cb->buf + cb->len, bytes, n);
    cb->len += n;
}

static void emit_movabs_rax(CodeBuf *cb, int64_t imm) {
    uint8_t b[10] = { 0x48, 0xB8 };
    memcpy(b + 2, &imm, 8);
    emit_bytes(cb, b, 10);
}
static void emit_push_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x50 }, 1); }
static void emit_pop_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x58 }, 1); }
static void emit_pop_rcx(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x59 }, 1); }
static void emit_sar3_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0xC1, 0xF8, 0x03 }, 4); }
static void emit_sar3_rcx(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0xC1, 0xF9, 0x03 }, 4); }
static void emit_add_rcx_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0x01, 0xC8 }, 3); }
static void emit_sub_rcx_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0x29, 0xC8 }, 3); }
static void emit_imul_rcx_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0x0F, 0xAF, 0xC1 }, 4); }
static void emit_shl3_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0xC1, 0xE0, 0x03 }, 4); }
static void emit_or1_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0x83, 0xC8, 0x01 }, 4); }
static void emit_shr3_rax(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0x48, 0xC1, 0xE8, 0x03 }, 4); }
static void emit_cmp_imm8_rax(CodeBuf *cb, uint8_t imm8) {
    emit_bytes(cb, (uint8_t[]){ 0x48, 0x83, 0xF8, imm8 }, 4);
}
static void emit_add_rsp_imm8(CodeBuf *cb, uint8_t imm8) {
    emit_bytes(cb, (uint8_t[]){ 0x48, 0x83, 0xC4, imm8 }, 4);
}
static void emit_ret(CodeBuf *cb) { emit_bytes(cb, (uint8_t[]){ 0xC3 }, 1); }

static void emit_load_rsp_disp32_rax(CodeBuf *cb, int32_t disp) {
    uint8_t b[8] = { 0x48, 0x8B, 0x84, 0x24 };
    memcpy(b + 4, &disp, 4);
    emit_bytes(cb, b, 8);
}

/* Returns the byte offset of the 4-byte rel32 *field itself* (not the
 * instruction start) so patch_rel32's "field_offset+4" math is uniform
 * across differently-sized opcodes. */
static size_t emit_jne_rel32_placeholder(CodeBuf *cb) {
    emit_bytes(cb, (uint8_t[]){ 0x0F, 0x85, 0, 0, 0, 0 }, 6);
    return cb->len - 4;
}
static size_t emit_jmp_rel32_placeholder(CodeBuf *cb) {
    emit_bytes(cb, (uint8_t[]){ 0xE9, 0, 0, 0, 0 }, 5);
    return cb->len - 4;
}
static void patch_rel32(CodeBuf *cb, size_t field_offset, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - (field_offset + 4));
    memcpy(cb->buf + field_offset, &rel, 4);
}

/* Let-bound values live as plain native-stack slots (no heap Env needed --
 * this subset has no closures to capture one). `stack_index` is the
 * binding's absolute 0-indexed position from the bottom of this function's
 * stack region; resolving a read means knowing how many slots are
 * currently live (`depth`) to compute the %rsp-relative displacement. */
typedef struct ScopeFrame2 {
    struct ScopeFrame2 *up;
    const char *name;
    size_t name_len;
    size_t stack_index;
} ScopeFrame2;

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

static void codegen_expr(CodeBuf *cb, Ast *node, ScopeFrame2 *scope, size_t depth);

static void codegen_var(CodeBuf *cb, Ast *node, ScopeFrame2 *scope, size_t depth) {
    for (ScopeFrame2 *f = scope; f; f = f->up) {
        if (name_eq(f->name, f->name_len, node->as.var.name, node->as.var.name_len)) {
            int32_t disp = (int32_t)((depth - 1 - f->stack_index) * 8);
            emit_load_rsp_disp32_rax(cb, disp);
            emit_push_rax(cb);
            return;
        }
    }
    fprintf(stderr, "asnix: undefined variable '%.*s' (M4.5 codegen)\n",
            (int)node->as.var.name_len, node->as.var.name);
    exit(1);
}

static void codegen_expr(CodeBuf *cb, Ast *node, ScopeFrame2 *scope, size_t depth) {
    switch (node->kind) {
        case AST_INT:
            emit_movabs_rax(cb, (int64_t)asnix_make_int(node->as.int_val).raw);
            emit_push_rax(cb);
            return;
        case AST_BOOL:
            emit_movabs_rax(cb, (int64_t)(node->as.bool_val ? ASNIX_TRUE : ASNIX_FALSE).raw);
            emit_push_rax(cb);
            return;
        case AST_NULL:
            emit_movabs_rax(cb, (int64_t)ASNIX_NULL.raw);
            emit_push_rax(cb);
            return;
        case AST_VAR:
            codegen_var(cb, node, scope, depth);
            return;
        case AST_LET: {
            codegen_expr(cb, node->as.let1.value, scope, depth); /* net +1 */
            ScopeFrame2 frame = { .up = scope, .name = node->as.let1.name,
                                   .name_len = node->as.let1.name_len, .stack_index = depth };
            codegen_expr(cb, node->as.let1.body, &frame, depth + 1); /* net +1 */
            emit_pop_rax(cb);         /* rax = body result */
            emit_add_rsp_imm8(cb, 8); /* discard the now-dead bound value */
            emit_push_rax(cb);        /* net effect of the whole AST_LET: +1 */
            return;
        }
        case AST_IF: {
            codegen_expr(cb, node->as.if1.cond, scope, depth); /* +1 */
            emit_pop_rax(cb);                                   /* -1, net 0 so far */
            emit_shr3_rax(cb);
            emit_cmp_imm8_rax(cb, ASNIX_IMM_TRUE);
            size_t jne_site = emit_jne_rel32_placeholder(cb); /* branch to else unless truthy */
            codegen_expr(cb, node->as.if1.then_, scope, depth); /* +1 */
            size_t jmp_site = emit_jmp_rel32_placeholder(cb);
            size_t else_pos = cb->len;
            patch_rel32(cb, jne_site, else_pos);
            codegen_expr(cb, node->as.if1.else_, scope, depth); /* +1 */
            size_t end_pos = cb->len;
            patch_rel32(cb, jmp_site, end_pos);
            return; /* both branches leave the stack at depth+1: net +1 either way */
        }
        case AST_BINOP:
            codegen_expr(cb, node->as.binop.lhs, scope, depth);     /* +1 */
            codegen_expr(cb, node->as.binop.rhs, scope, depth + 1); /* +1 */
            emit_pop_rcx(cb); /* rhs */
            emit_pop_rax(cb); /* lhs -- net -2 so far, depth back to original */
            emit_sar3_rax(cb);
            emit_sar3_rcx(cb);
            switch (node->as.binop.op) {
                case '+': emit_add_rcx_rax(cb); break;
                case '-': emit_sub_rcx_rax(cb); break;
                case '*': emit_imul_rcx_rax(cb); break;
            }
            emit_shl3_rax(cb);
            emit_or1_rax(cb);
            emit_push_rax(cb); /* net +1 overall */
            return;
        case AST_LAMBDA:
        case AST_APP:
            fprintf(stderr,
                    "asnix: M4.5 native codegen does not yet support lambdas/calls "
                    "(needs a heap closure + call/return protocol -- see codegen.h)\n");
            exit(1);
        case AST_STRING:
        case AST_LIST:
        case AST_ATTRSET:
        case AST_SELECT:
        case AST_HAS_ATTR:
            fprintf(stderr,
                    "asnix: M4.5 native codegen does not yet support strings/lists/attrsets "
                    "(M5 feature, interpreter-only so far -- see docs/architecture.md)\n");
            exit(1);
    }
}

AsnixNativeFn asnix_codegen(Ast *root) {
    CodeBuf cb = { 0 };
    codegen_expr(&cb, root, NULL, 0);
    emit_pop_rax(&cb);
    emit_ret(&cb);

    long pagesz = sysconf(_SC_PAGESIZE);
    size_t maplen = ((cb.len + (size_t)pagesz - 1) / (size_t)pagesz) * (size_t)pagesz;

    void *mem = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "asnix: mmap failed in M4.5 codegen\n");
        exit(1);
    }
    memcpy(mem, cb.buf, cb.len);
    free(cb.buf);

    /* W^X: never hold PROT_WRITE and PROT_EXEC simultaneously. */
    if (mprotect(mem, maplen, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "asnix: mprotect failed in M4.5 codegen\n");
        exit(1);
    }

    return (AsnixNativeFn)mem;
}
