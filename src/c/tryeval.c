#include "tryeval.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "asnix/ir.h"

static jmp_buf *g_tryeval_stack[ASNIX_TRYEVAL_MAX_DEPTH];
static int g_tryeval_depth = 0;

extern int64_t asnix_inprogress_head;

int64_t asnix_tryeval_save_inprogress_head(void) {
    return asnix_inprogress_head;
}

void asnix_tryeval_unwind_inprogress(int64_t saved_head) {
    int64_t cur = asnix_inprogress_head;
    while (cur != saved_head) {
        char *cell = (char *)(intptr_t)cur;
        int64_t next = *(int64_t *)(cell + ASNIX_THUNK_OFF_ENV);
        *(int64_t *)(cell + ASNIX_THUNK_OFF_STATE) = ASNIX_THUNK_STATE_FAILED;
        cur = next;
    }
    asnix_inprogress_head = saved_head;
}

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

