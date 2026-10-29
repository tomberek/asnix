#ifndef ASNIX_EVAL_FILE_H
#define ASNIX_EVAL_FILE_H

#include "asnix/value.h"

Value asnix_eval_file(const char *path);

Value asnix_eval_file_scoped(const char *path, Value scope);

Value asnix_eval_expr_source(const char *src);

Value asnix_eval_file_for_derivation_bootstrap(const char *path);

const char *asnix_eval_top_level_file(void);

#endif

