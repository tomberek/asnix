#ifndef ASNIX_ATTRS_UPDATE_H
#define ASNIX_ATTRS_UPDATE_H

#include "asnix/value.h"

Value asnix_attrs_update(Value lhs, Value rhs);

Value asnix_attrs_eager_merge(Value lhs, Value rhs);

Value asnix_attrs_insert_dynamic(Value attrs, Value name_val, Value value);

#endif

