#ifndef ASNIX_BUILTINS_SHARED_H
#define ASNIX_BUILTINS_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/env.h"
#include "asnix/value.h"
#include "builtins_list.h"

Value asnix_bshared_make_primop(void *fn, int64_t arity);

Value asnix_bshared_make_primop_noforce(void *fn, int64_t arity);

Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity);

Value asnix_bshared_make_curried_native_noforce(void *native_fn, int64_t arity);

Value asnix_bshared_native_formals_marker(void);

Value asnix_bshared_apply2(Value fn, Value a, Value b);

void asnix_bshared_fatal(const char *who, const char *msg);
_Noreturn void asnix_bshared_select_missing_fatal(uint32_t sym);

const char *asnix_bshared_type_name(Value v);

_Noreturn void asnix_bshared_select_type_fatal(Value v);

_Noreturn void asnix_bshared_call_type_fatal(Value v);

Value asnix_bshared_call_functor(Value attrs_val, Value arg);

_Noreturn void asnix_bshared_cf_missing_fatal(uint32_t sym);
_Noreturn void asnix_bshared_cf_extra_fatal(uint32_t sym);

void asnix_bshared_require_tag(Value v, int tag, const char *who, const char *what);
#define ASNIX_REQUIRE_ATTRS(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_ATTRS, (who), "an attrset")
#define ASNIX_REQUIRE_STRING(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_PTR, (who), "a string")
#define ASNIX_REQUIRE_LIST(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_LIST, (who), "a list")
#define ASNIX_REQUIRE_INT(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_INT, (who), "an integer")

int64_t asnix_bshared_list_count(Value v);
const Value *asnix_bshared_list_elems(Value v);
Value asnix_bshared_make_list(const Value *elems, int64_t count);

int64_t asnix_bshared_attrs_count(Value v);
uint32_t asnix_bshared_attrs_sym(Value v, int64_t i);
Value asnix_bshared_attrs_value(Value v, int64_t i);
int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym);

int asnix_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value);

Value asnix_bshared_attrs_resolve(Value v);

Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count);

Value asnix_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta);

void asnix_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who);

int asnix_bshared_value_eq(Value a, Value b);

int asnix_bshared_value_eq_op(Value a, Value b);

int asnix_bshared_value_lt(Value a, Value b, const char *who);

Value asnix_bshared_select_dynamic(Value target, Value name_val);

int asnix_bshared_select_dynamic_or(Value target, Value name_val, Value *out);

int asnix_bshared_has_attr_dynamic(Value target, Value name_val);

#endif

