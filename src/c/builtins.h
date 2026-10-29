#ifndef ASNIX_BUILTINS_H
#define ASNIX_BUILTINS_H

#include "asnix/value.h"

Value asnix_make_builtins_attrs(void);

Value asnix_make_builtins_attrs_for_derivation_bootstrap(void);

Value asnix_make_import_primop(void);

Value asnix_make_abort_primop(void);

Value asnix_make_throw_primop(void);

Value asnix_make_toString_primop(void);

#endif

