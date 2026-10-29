#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static int string_eq(Value v, const char *s) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(base + ASNIX_STRING_OFF_LEN);
    size_t slen = strlen(s);
    return (size_t)len == slen && memcmp(base + ASNIX_STRING_OFF_BYTES, s, slen) == 0;
}

static void fixture_concat_all_strings(void) {
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)asnix_make_string_value("a", 1).raw);
    ib_push_const(b, (int64_t)asnix_make_string_value("b", 1).raw);
    ib_push_const(b, (int64_t)asnix_make_string_value("c", 1).raw);
    ib_str_concat(b, 3);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, NULL);
    CHECK(string_eq(v, "abc"), "concatenating 3 string parts yields \"abc\"");
}

static void fixture_concat_already_stringified_int(void) {
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)asnix_make_string_value("x=", 2).raw);
    ib_push_const(b, (int64_t)asnix_coerce_to_string(asnix_make_int(7)).raw);
    ib_push_const(b, (int64_t)asnix_make_string_value("!", 1).raw);
    ib_str_concat(b, 3);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, NULL);
    CHECK(string_eq(v, "x=7!"), "concatenating an already-toString'd int part works");
}

static int child_concat_raw_int_aborts(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 7);
    ib_str_concat(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    asnix_eval(prog, NULL);
    return 0;
}

static void fixture_concat_rejects_raw_int(void) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        asnix_arena_init(0);
        _exit(child_concat_raw_int_aborts());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          "interpolating a raw (non-toString'd) int is a fatal error, matching real Nix");
}

static void fixture_concat_empty(void) {
    IrBuilder *b = ib_new();
    ib_str_concat(b, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, NULL);
    CHECK(string_eq(v, ""), "OP_STR_CONCAT with count=0 yields an empty string, not a crash");
}

static void fixture_concat_coerces_path(void) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }
    char self_path[4096 + 64];
    snprintf(self_path, sizeof(self_path), "%s/tests/ir/test_strings_fixtures.c", cwd);

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)asnix_make_string_value("p=", 2).raw);
    ib_push_const(b, (int64_t)asnix_make_path_value(self_path, strlen(self_path)).raw);
    ib_push_const(b, (int64_t)asnix_make_string_value("!", 1).raw);
    ib_str_concat(b, 3);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, NULL);

    const char *prefix = "p=/nix/store/";
    const char *suffix = "test_strings_fixtures.c!";
    int64_t vlen;
    const char *vbytes = asnix_ptr_bytes(v, &vlen);
    int matches = vlen > (int64_t)(strlen(prefix) + strlen(suffix)) &&
                  memcmp(vbytes, prefix, strlen(prefix)) == 0 &&
                  memcmp(vbytes + vlen - (int64_t)strlen(suffix), suffix, strlen(suffix)) == 0;
    CHECK(matches, "interpolating a Path copies it to the store, not a crash");
}

int main(void) {
    asnix_arena_init(0);
    fixture_concat_all_strings();
    fixture_concat_already_stringified_int();
    fixture_concat_rejects_raw_int();
    fixture_concat_empty();
    fixture_concat_coerces_path();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_strings_fixtures: all checks passed\n");
    return 0;
}

