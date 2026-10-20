/*
 * Regression coverage for `abort` (and the bare-global exposure of
 * `import`/`map`/`abort` alongside `builtins.X`, added in the same pass --
 * see compiler.c's root ScopeFrame and eval_file.c's root Env for the
 * actual mechanism). `abort` unconditionally exits the process, so this
 * follows the same fork()+exit-code pattern already established by
 * tests/asm/test_stacklimits.c and tests/c/test_apply.c for abort paths.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/ir.h"
#include "builtins.h"
#include "builtins_list.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* `abort "msg"` must exit 1 and print real Nix's core wording (confirmed
 * directly against nix-instantiate: "evaluation aborted with the
 * following error message: '<msg>'" -- the "while calling" trace context
 * real Nix also prints isn't reproduced, same simplification every other
 * fatal error in this project already makes). */
static void test_abort_exits_with_message(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { CHECK(0, "pipe() failed"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 2); /* redirect stderr into the pipe */
        close(pipefd[1]);
        asnix_arena_init(0);
        Value args[1] = { asnix_make_string_value("custom message", 15) };
        Value primop = asnix_make_abort_primop();
        asnix_apply(primop, args, 1);
        _exit(42); /* reaching here means abort failed to trigger */
    }
    close(pipefd[1]);
    char buf[256] = { 0 };
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, "abort exits with code 1, not a crash");
    CHECK(n > 0 && strstr(buf, "custom message") != NULL,
          "abort's stderr message names the actual aborted-with text");
}

int main(void) {
    test_abort_exits_with_message();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_abort: all checks passed\n");
    return 0;
}
