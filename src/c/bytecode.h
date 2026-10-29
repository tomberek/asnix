#ifndef ASNIX_BYTECODE_H
#define ASNIX_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/ir.h"

int asnix_bytecode_serialize(IrProgram *prog, uint8_t **out_buf, size_t *out_len);

IrProgram *asnix_bytecode_deserialize(const uint8_t *buf, size_t len);

#endif

