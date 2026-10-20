#include <stdio.h>
#include <stdlib.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"

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

static void print_value(Value v) {
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            printf("%lld\n", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: printf("true\n"); return;
                case ASNIX_IMM_FALSE: printf("false\n"); return;
                case ASNIX_IMM_NULL: printf("null\n"); return;
                default: printf("<imm %d>\n", asnix_imm_subtag(v)); return;
            }
        default:
            printf("<unprintable value, tag=%d>\n", asnix_tag_of(v));
            return;
    }
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

    Env *root_env = asnix_arena_alloc(8);
    root_env->up = NULL;

    Value result = asnix_eval(prog, root_env);
    print_value(result);
    return 0;
}
