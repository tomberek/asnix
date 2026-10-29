#ifndef ASNIX_TOML_H
#define ASNIX_TOML_H

#include <stddef.h>

#include "asnix/value.h"

Value asnix_toml_parse(const char *text, size_t len);

#endif

