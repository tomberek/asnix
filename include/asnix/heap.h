#ifndef ASNIX_HEAP_H
#define ASNIX_HEAP_H

#define ASNIX_STRING_OFF_LEN   0
#define ASNIX_STRING_OFF_BYTES 8

#define ASNIX_PATH_MARKER     (-1)
#define ASNIX_PATH_OFF_MARKER 0
#define ASNIX_PATH_OFF_LEN    8
#define ASNIX_PATH_OFF_BYTES  16

#define ASNIX_FLOAT_MARKER     (-2)
#define ASNIX_FLOAT_OFF_MARKER 0
#define ASNIX_FLOAT_OFF_VALUE  8

/* M199: Cell is the one TAG_PTR sub-kind whose single pointer field is
 * PROVABLY always a pointer into the bounded, 32-bit-offset-addressable
 * Env arena (see env.h's ASNIX_ENV_ARENA_SIZE and M119) -- every
 * construction site (op_var_ref_cell.S) takes the address of an
 * ASNIX_ENV_OFF_VALUES slot and nothing else ever writes a Cell's
 * target. That makes it the one heap-resident kind in this project
 * where M198's "env/target field needs a full unconstrained 8 bytes"
 * disqualifier (true for Thunk.env, Cell's own closest analogue)
 * does NOT apply: the marker shrinks from a 64-bit field to a 32-bit
 * one (still compared via a sign-extending movslq, so every OTHER
 * TAG_PTR sub-kind's marker check is bit-for-bit unchanged) and the
 * target shrinks from a raw 8-byte pointer to a 32-bit env-arena
 * offset, added to asnix_env_arena_base at each chase site. Total Cell
 * footprint: 16 bytes -> 8 bytes. See docs/architecture.md M199.
 */
#define ASNIX_CELL_MARKER     (-3)
#define ASNIX_CELL_OFF_MARKER 0
#define ASNIX_CELL_OFF_TARGET 4

#define ASNIX_CELL_CHASE_LIMIT 1000000

#define ASNIX_LAZYAPP_MARKER        (-4)
#define ASNIX_LAZYAPP_FORCED_MARKER (-5)
#define ASNIX_LAZYAPP_OFF_MARKER    0
#define ASNIX_LAZYAPP_OFF_FN        8
#define ASNIX_LAZYAPP_OFF_RESULT    8
#define ASNIX_LAZYAPP_OFF_ARG1      16
#define ASNIX_LAZYAPP_OFF_ARG2      24
#define ASNIX_LAZYAPP_SIZE          32

#define ASNIX_THUNKMAP_MARKER         (-6)
#define ASNIX_THUNKMAP_OFF_MARKER     0
#define ASNIX_THUNKMAP_OFF_F          8
#define ASNIX_THUNKMAP_OFF_SRC_ENTRIES 16
#define ASNIX_THUNKMAP_OFF_OUT        24
#define ASNIX_THUNKMAP_SIZE           32
#define ASNIX_THUNKMAP_MAX_IDX        0xFFFF

#define ASNIX_LIST_OFF_COUNT 0
#define ASNIX_LIST_OFF_ELEMS 8

#define ASNIX_ATTRS_OFF_COUNT   0
#define ASNIX_ATTRS_OFF_ENTRIES 8
#define ASNIX_ATTR_OFF_SYM      0
#define ASNIX_ATTR_OFF_VALUE    4
#define ASNIX_ATTR_SIZE         12

#define ASNIX_ATTRS_UNION_OFF_NLEAVES   0
#define ASNIX_ATTRS_UNION_OFF_FLATCACHE 8
#define ASNIX_ATTRS_UNION_OFF_LEAVES    16
#define ASNIX_ATTRS_UNION_MAX_LEAVES    64

#define ASNIX_PRIMOP_OFF_FN      0
#define ASNIX_PRIMOP_OFF_ARITY   8
#define ASNIX_PRIMOP_OFF_NOFORCE 16
#define ASNIX_PRIMOP_SIZE        24

#endif

