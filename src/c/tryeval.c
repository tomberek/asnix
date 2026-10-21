#include "tryeval.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static jmp_buf *g_tryeval_stack[ASNIX_TRYEVAL_MAX_DEPTH];
static int g_tryeval_depth = 0;

void asnix_tryeval_push(jmp_buf *buf) {
    if (g_tryeval_depth >= ASNIX_TRYEVAL_MAX_DEPTH) {
        fprintf(stderr, "asnix: tryEval nesting too deep, aborting\n");
        exit(1);
    }
    g_tryeval_stack[g_tryeval_depth++] = buf;
}

void asnix_tryeval_pop(void) {
    g_tryeval_depth--;
}

_Noreturn void asnix_tryeval_raise_or_fatal(const char *msg, size_t len) {
    if (g_tryeval_depth > 0) {
        longjmp(*g_tryeval_stack[g_tryeval_depth - 1], 1);
    }
    write(2, msg, len);
    exit(1);
}
