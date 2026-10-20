#ifndef ASNIX_HEAP_H
#define ASNIX_HEAP_H

/*
 * Heap object layouts for the ASNIX_TAG_PTR (string), ASNIX_TAG_LIST, and
 * ASNIX_TAG_ATTRS value kinds (see docs/value-encoding.md). All arena-
 * allocated, never freed, 8-byte aligned.
 */

/* String: {int64_t len; char bytes[len];} -- the only ASNIX_TAG_PTR kind
 * so far, so no in-object Kind-discriminant byte is needed yet (see the
 * note in value.h about why that tag is reserved as the generic fallback). */
#define ASNIX_STRING_OFF_LEN   0
#define ASNIX_STRING_OFF_BYTES 8

/* List: {int64_t count; Value elems[count];} */
#define ASNIX_LIST_OFF_COUNT 0
#define ASNIX_LIST_OFF_ELEMS 8

/* Attrs: {int64_t count; Attr entries[count];}, entries sorted by sym --
 * binary-searched by OP_SELECT. Each Attr is {uint32_t sym; uint32_t pad;
 * Value value;} = 16 bytes; sym+pad are read/written together as one
 * 8-byte field (the pad is always zero, since the compiler stores symbol
 * ids as plain non-negative int64_t in the constant pool -- see
 * src/c/compiler.c and op_mk_attrs.S). */
#define ASNIX_ATTRS_OFF_COUNT   0
#define ASNIX_ATTRS_OFF_ENTRIES 8
#define ASNIX_ATTR_OFF_SYM      0
#define ASNIX_ATTR_OFF_VALUE    8
#define ASNIX_ATTR_SIZE         16

/* PrimOp (M7): {void *fn; int64_t arity;} -- a native builtin function.
 * `fn` has signature `Value (*)(const Value *args, int64_t argc)`; OP_CALL
 * invokes it directly with a pointer straight into VALSTACK (the same
 * contiguous arg layout the Closure call path already reads from) rather
 * than copying into a new Env frame -- primops have no lexical body to
 * capture a scope for. */
#define ASNIX_PRIMOP_OFF_FN    0
#define ASNIX_PRIMOP_OFF_ARITY 8
#define ASNIX_PRIMOP_SIZE      16

#endif /* ASNIX_HEAP_H */
