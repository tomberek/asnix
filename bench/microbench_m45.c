/* Ad hoc microbenchmark: M3/M4 threaded-interpreter dispatch vs M4.5
 * generated-native execution, isolating *dispatch* overhead by timing only
 * repeated execution of an already-compiled program/already-generated
 * function (lexing/parsing/codegen cost is paid once, outside the timed
 * loop). Not part of `make test` -- this is a one-off measurement, not a
 * pass/fail gate; see docs/architecture.md's M4.5 acceptance notes and the
 * M9 roadmap milestone for the real benchmark harness this will grow into.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/value.h"
#include "ast.h"
#include "codegen.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void bench_one(const char *label, const char *src, long iters) {
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks);

    IrProgram *prog = asnix_compile(ast, "."); /* no path literals in these workloads; base_dir is unused */
    Env *root = asnix_arena_alloc(8);
    root->up = NULL;

    AsnixNativeFn fn = asnix_codegen(ast);

    volatile int64_t sink = 0;

    double t0 = now_seconds();
    for (long i = 0; i < iters; i++) sink += asnix_int_value(asnix_eval(prog, root));
    double t_interp = now_seconds() - t0;

    double t1 = now_seconds();
    for (long i = 0; i < iters; i++) sink += asnix_int_value(fn());
    double t_native = now_seconds() - t1;

    printf("%-20s interp=%8.1fns/call  native=%8.1fns/call  speedup=%.2fx\n", label,
           t_interp * 1e9 / (double)iters, t_native * 1e9 / (double)iters, t_interp / t_native);
}

int main(void) {
    /* asnix_eval allocates a fresh VALSTACK+RETSP (never freed) on every
     * call -- fine for one-shot evaluation, but it means a tight
     * micro-benchmark loop must keep iters modest or it'll exhaust memory
     * long before dispatch-overhead differences matter. Arena reuse across
     * calls is a real optimization opportunity, not implemented here. */
    asnix_arena_init(0);
    long iters = 50000;
    bench_one("arith (2+3*4)", "2 + 3 * 4", iters);
    bench_one("let x=10;y=20", "let x = 10; y = 20; in x + y", iters);
    bench_one("nested_let", "let x = 1; in let y = 2; in x + y", iters);
    bench_one("if_true", "if true then 1 else 2", iters);
    return 0;
}
