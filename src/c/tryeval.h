#ifndef ASNIX_TRYEVAL_H
#define ASNIX_TRYEVAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define ASNIX_TRYEVAL_MAX_DEPTH 256

void asnix_tryeval_push(jmp_buf *buf);

void asnix_tryeval_pop(void);

_Noreturn void asnix_tryeval_raise_or_fatal(const char *msg, size_t len);

int64_t asnix_tryeval_save_inprogress_head(void);

void asnix_tryeval_unwind_inprogress(int64_t saved_head);

#endif

