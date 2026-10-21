/* Strings-milestone validation, same strategy as M3/M5/M6/M7: exercise
 * OP_STR_CONCAT's pop-N/coerce/concatenate path via hand-fed IR before the
 * lexer/parser/compiler changes for real "foo ${expr} bar" syntax depend
 * on it. */
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

/* "a" ++ "b" ++ "c" (all already strings) == "abc" */
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

/* "x=" ++ toString(7) ++ "!" == "x=7!" -- the *compiler's* job, when it
 * sees an explicit `builtins.toString` call in an interpolated part, is
 * to emit that call's IR so OP_STR_CONCAT only ever receives an
 * already-string value here. */
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

/* Interpolating a *raw*, non-stringified INT/BOOL/NULL must be a hard
 * error, NOT silently coerced -- verified directly against real Nix
 * (`nix-instantiate --eval -E '"${7}"'`/`'"${true}"'`/`'"${null}"'` all
 * fail with "cannot coerce ... to a string", even though `builtins.
 * toString 7`/`true`/`null` succeed -- see value_util.c's
 * coerce_to_string_for_interp comment). Since the rejection path calls
 * exit(1) (matching this project's existing abort-the-process convention
 * for every other "cannot coerce"/fatal-type-error site), this has to be
 * checked out-of-process via fork(), not a plain CHECK(). */
static int child_concat_raw_int_aborts(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 7);
    ib_str_concat(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    asnix_eval(prog, NULL); /* expected to exit(1) before returning */
    return 0;
}

static void fixture_concat_rejects_raw_int(void) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2); /* silence the expected fatal-error message on stderr */
        asnix_arena_init(0);
        _exit(child_concat_raw_int_aborts());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          "interpolating a raw (non-toString'd) int is a fatal error, matching real Nix");
}

/* Zero-length concat (an empty "" literal with no parts at all would
 * normally be folded to a constant at compile time -- but the op itself
 * must not misbehave if ever asked to pop 0). */
static void fixture_concat_empty(void) {
    IrBuilder *b = ib_new();
    ib_str_concat(b, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, NULL);
    CHECK(string_eq(v, ""), "OP_STR_CONCAT with count=0 yields an empty string, not a crash");
}

/* "p=" ++ <a Path value> ++ "!" == "p=<selfpath>!" -- regression for a
 * real integration bug (docs/architecture.md's Known bugs #13): Path is
 * a second, differently-shaped ASNIX_TAG_PTR kind (added by the
 * separately-landed path-literals milestone), and the interpolation
 * coercion path used to treat every ASNIX_TAG_PTR as an already-valid
 * String and return it unchanged -- reading a Path's marker field as a
 * bogus string length and segfaulting. Originally used the portable,
 * always-exists "/" here (M67 added a real existence check to this
 * coercion path, so the path needs to exist) -- M75 made this coercion
 * ALSO copy the path to the store (not just check existence), and "/"
 * being the actual store-copy input tries to NAR-serialize the entire
 * filesystem root, which fails even on real Nix on this machine
 * (permission denied under /boot, confirmed directly) -- same bug M75
 * already found and fixed in tests/e2e/m67_path_interp_exists.nix.
 * Self-references this very source file instead (via `getcwd()`,
 * since `make test` always runs test binaries from the repo root) --
 * small, always exists, no permission issues. */
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
