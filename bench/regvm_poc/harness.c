/* Driver for the register-based-VM POC microbenchmark (chains.S). Builds
 * one real asnix Env (env.h's exact packed layout) holding 8 tagged ints,
 * builds a 16-entry tape (ir.h's exact 24-byte {handler,a,b} record
 * layout) for each of the two chains, and runs ONE of the two entry
 * points `iters` times based on argv[1] ("stack" or "regvm") -- a
 * SEPARATE PROCESS per variant is the point: `perf stat` on the whole
 * process attributes essentially 100% of its instructions/cycles to the
 * chosen chain, with no risk of one variant's branch-predictor/cache
 * state bleeding into the other's measurement.
 *
 * argv[2] (optional): iteration count, default 200,000,000.
 *
 * Correctness: prints the checksum (XOR-fold of every iteration's final
 * LT result) -- compare `stack` vs `regvm`'s printed checksum for the
 * SAME iters value; they must be bit-identical, since both chains
 * compute the exact same (a+b)*(c-d) < (e+f)*(g-h) on the exact same
 * data every iteration. This is the whole correctness gate for this POC
 * (not wired into `make test` -- see README.md).
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"

typedef struct {
    void *handler;
    int64_t a;
    int64_t b;
} BenchOp;

extern void *regvm_poc_handler_table[18];
extern uint64_t run_valstack_chain(Env *env, long iters, BenchOp *tape);
extern uint64_t run_regvm_chain(Env *env, long iters, BenchOp *tape);

#define H_VA_VAR_REF   regvm_poc_handler_table[0]
#define H_VA_ADD       regvm_poc_handler_table[1]
#define H_VA_SUB       regvm_poc_handler_table[2]
#define H_VA_MUL       regvm_poc_handler_table[3]
#define H_VA_LT        regvm_poc_handler_table[4]
#define H_VA_LOOPCTL   regvm_poc_handler_table[5]
#define H_RV_VR_R8     regvm_poc_handler_table[6]
#define H_RV_VR_R9     regvm_poc_handler_table[7]
#define H_RV_VR_R10    regvm_poc_handler_table[8]
#define H_RV_VR_R11    regvm_poc_handler_table[9]
#define H_RV_ADD_R8_R9   regvm_poc_handler_table[10]
#define H_RV_ADD_R9_R10  regvm_poc_handler_table[11]
#define H_RV_SUB_R9_R10  regvm_poc_handler_table[12]
#define H_RV_SUB_R10_R11 regvm_poc_handler_table[13]
#define H_RV_MUL_R8_R9   regvm_poc_handler_table[14]
#define H_RV_MUL_R9_R10  regvm_poc_handler_table[15]
#define H_RV_LT_R8_R9    regvm_poc_handler_table[16]
#define H_RV_LOOPCTL     regvm_poc_handler_table[17]

#define TAGI(n) (((uint64_t)(n) << ASNIX_TAG_BITS) | ASNIX_TAG_INT)

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* (a + b) * (c - d) < (e + f) * (g - h), slots 0..7 = a..h. */
static const int64_t kVals[8] = { 3, 5, 20, 4, 7, 2, 10, 3 };

static void build_stack_tape(BenchOp *t) {
    int i = 0;
    for (int slot = 0; slot < 2; slot++) { t[i].handler = H_VA_VAR_REF; t[i].a = 0; t[i].b = slot; i++; }
    t[i].handler = H_VA_ADD; t[i].a = 0; t[i].b = 0; i++;
    for (int slot = 2; slot < 4; slot++) { t[i].handler = H_VA_VAR_REF; t[i].a = 0; t[i].b = slot; i++; }
    t[i].handler = H_VA_SUB; t[i].a = 0; t[i].b = 0; i++;
    t[i].handler = H_VA_MUL; t[i].a = 0; t[i].b = 0; i++;
    for (int slot = 4; slot < 6; slot++) { t[i].handler = H_VA_VAR_REF; t[i].a = 0; t[i].b = slot; i++; }
    t[i].handler = H_VA_ADD; t[i].a = 0; t[i].b = 0; i++;
    for (int slot = 6; slot < 8; slot++) { t[i].handler = H_VA_VAR_REF; t[i].a = 0; t[i].b = slot; i++; }
    t[i].handler = H_VA_SUB; t[i].a = 0; t[i].b = 0; i++;
    t[i].handler = H_VA_MUL; t[i].a = 0; t[i].b = 0; i++;
    t[i].handler = H_VA_LT; t[i].a = 0; t[i].b = 0; i++;
    t[i].handler = H_VA_LOOPCTL; t[i].a = 0; t[i].b = 0; i++;
    if (i != 16) { fprintf(stderr, "internal error: stack tape length %d != 16\n", i); exit(1); }
}

static void build_regvm_tape(BenchOp *t) {
    int i = 0;
    t[i].handler = H_RV_VR_R8; t[i].a = 0; t[i].b = 0; i++;   // a -> r8
    t[i].handler = H_RV_VR_R9; t[i].a = 0; t[i].b = 1; i++;   // b -> r9
    t[i].handler = H_RV_ADD_R8_R9; t[i].a = 0; t[i].b = 0; i++; // r8 = a+b
    t[i].handler = H_RV_VR_R9; t[i].a = 0; t[i].b = 2; i++;   // c -> r9
    t[i].handler = H_RV_VR_R10; t[i].a = 0; t[i].b = 3; i++;  // d -> r10
    t[i].handler = H_RV_SUB_R9_R10; t[i].a = 0; t[i].b = 0; i++; // r9 = c-d
    t[i].handler = H_RV_MUL_R8_R9; t[i].a = 0; t[i].b = 0; i++; // r8 = (a+b)*(c-d)
    t[i].handler = H_RV_VR_R9; t[i].a = 0; t[i].b = 4; i++;   // e -> r9
    t[i].handler = H_RV_VR_R10; t[i].a = 0; t[i].b = 5; i++;  // f -> r10
    t[i].handler = H_RV_ADD_R9_R10; t[i].a = 0; t[i].b = 0; i++; // r9 = e+f
    t[i].handler = H_RV_VR_R10; t[i].a = 0; t[i].b = 6; i++;  // g -> r10
    t[i].handler = H_RV_VR_R11; t[i].a = 0; t[i].b = 7; i++;  // h -> r11
    t[i].handler = H_RV_SUB_R10_R11; t[i].a = 0; t[i].b = 0; i++; // r10 = g-h
    t[i].handler = H_RV_MUL_R9_R10; t[i].a = 0; t[i].b = 0; i++; // r9 = (e+f)*(g-h)
    t[i].handler = H_RV_LT_R8_R9; t[i].a = 0; t[i].b = 0; i++;   // r8 = r8 < r9
    t[i].handler = H_RV_LOOPCTL; t[i].a = 0; t[i].b = 0; i++;
    if (i != 16) { fprintf(stderr, "internal error: regvm tape length %d != 16\n", i); exit(1); }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s stack|regvm [iters]\n", argv[0]);
        return 2;
    }
    long iters = (argc >= 3) ? strtol(argv[2], NULL, 10) : 200000000L;

    Env *env = malloc(sizeof(uint32_t) + 8 * sizeof(Value));
    env->up = 0;
    for (int i = 0; i < 8; i++) env->values[i].raw = TAGI(kVals[i]);

    BenchOp stack_tape[16];
    BenchOp regvm_tape[16];
    build_stack_tape(stack_tape);
    build_regvm_tape(regvm_tape);

    double t0 = now_seconds();
    uint64_t checksum;
    if (strcmp(argv[1], "stack") == 0) {
        checksum = run_valstack_chain(env, iters, stack_tape);
    } else if (strcmp(argv[1], "regvm") == 0) {
        checksum = run_regvm_chain(env, iters, regvm_tape);
    } else {
        fprintf(stderr, "unknown variant %s (want stack|regvm)\n", argv[1]);
        return 2;
    }
    double t1 = now_seconds();

    fprintf(stderr, "%s: iters=%ld checksum=0x%016lx wall=%.4fs (%.2f ns/iter)\n",
            argv[1], iters, (unsigned long)checksum, t1 - t0, (t1 - t0) * 1e9 / (double)iters);
    /* Use checksum as the exit code's low byte too, purely so nothing
     * about `checksum` can be considered dead by anything downstream
     * (e.g. a future wrapper script) without also changing program
     * behavior -- belt-and-suspenders against call elimination, on top
     * of the fprintf above which already forces it live. */
    return (int)(checksum & 0x1);
}
