#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "builtins.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "symtab.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void print_value_inner(Value v) {
    v = asnix_force_for_print(v); /* list elements and attrset values are lazy (M6) */
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
            const char *s = (const char *)(v.raw & ~0x7ULL);
            int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
            printf("\"%.*s\"", (int)len, s + ASNIX_STRING_OFF_BYTES);
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
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
            printf("{ ");
            for (int64_t i = 0; i < count; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
                Value val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
                size_t namelen;
                const char *name = asnix_sym_name(sym, &namelen);
                printf("%.*s = ", (int)namelen, name);
                print_value_inner(val);
                printf("; ");
            }
            printf("}");
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

static void print_value(Value v) {
    print_value_inner(v);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.nix>\n", argv[0]);
        return 1;
    }

    asnix_arena_init(0);

    char *src = read_file(argv[1]);
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks);
    IrProgram *prog = asnix_compile(ast);

    Env *root_env = asnix_env_alloc(NULL, 1);
    root_env->values[0] = asnix_make_builtins_attrs();

    Value result = asnix_eval(prog, root_env);
    print_value(result);
    return 0;
}
