/* M4.5 acceptance test: for every fixture this milestone's codegen
 * supports, the native-compiled path must produce a result byte-identical
 * to both the expected literal and the M3/M4 threaded-interpreter path --
 * three-way agreement, per docs/architecture.md's M4.5 acceptance
 * criterion (lambda/call fixtures are out of scope here; see codegen.h). */
#include <stdio.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/value.h"
#include "ast.h"
#include "codegen.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Ast *parse_source(const char *src) {
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    return asnix_parse(toks, ntoks, src);
}

static void check_fixture(const char *name, const char *src, int64_t expected) {
    Ast *ast = parse_source(src);

    IrProgram *prog = asnix_compile(ast, ".", NULL); /* no path literals in these fixtures; base_dir is unused */
    Env *root = asnix_arena_alloc(8);
    root->up = NULL;
    Value interpreted = asnix_eval(prog, root);

    AsnixNativeFn fn = asnix_codegen(ast);
    Value native = fn();

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: interpreted result == expected", name);
    CHECK(asnix_tag_of(interpreted) == ASNIX_TAG_INT && asnix_int_value(interpreted) == expected, buf);

    snprintf(buf, sizeof(buf), "%s: native-codegen result == expected", name);
    CHECK(asnix_tag_of(native) == ASNIX_TAG_INT && asnix_int_value(native) == expected, buf);

    snprintf(buf, sizeof(buf), "%s: interpreted and native agree exactly", name);
    CHECK(asnix_values_equal(interpreted, native), buf);
}

int main(void) {
    asnix_arena_init(0);

    check_fixture("arith", "2 + 3 * 4", 14);
    check_fixture("if_true", "if true then 1 else 2", 1);
    check_fixture("if_false", "if false then 1 else 2", 2);
    check_fixture("let", "let x = 10; y = 20; in x + y", 30);
    check_fixture("nested_let", "let x = 1; in let y = 2; in x + y", 3);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m45_codegen: all checks passed\n");
    return 0;
}
