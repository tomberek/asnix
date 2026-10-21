#ifndef ASNIX_FORCE_SCRATCH_H
#define ASNIX_FORCE_SCRATCH_H

/*
 * Sizes of the two per-call scratch buffers src/asm/force.S's
 * asnix_force_for_print carves out to run its own self-contained mini
 * dispatch context -- shared with force_scratch.c (below) so the two
 * never drift apart, same dual-use-header convention as arena.h's
 * ASNIX_ARENA_OFF_* constants.
 */
#define ASNIX_FORCE_VALSTACK_BYTES 262144 /* 32,768 Values, 256 KiB */
#define ASNIX_FORCE_RETSTACK_BYTES 65536  /* 2,048 call frames, 64 KiB */

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * asnix_force_for_print's nested-recursion depth (src/asm/force.S's
 * asnix_force_depth: 0 for a top-level, non-nested call; N for a call
 * reached from inside N other still-active force_for_print frames)
 * doubles as an index into a per-depth cache of scratch buffers here --
 * found chasing d.nix's own real-workload slowness: every call used to
 * mint a BRAND NEW pair of buffers from the never-free bump arena
 * (confirmed via `perf stat`: ~169k page faults / ~20 GB of one-shot
 * arena growth evaluating d.nix, ~33% of all page faults attributed
 * directly to asnix_force_for_print itself). A single depth==0-only
 * cache (the first attempt at fixing this) barely helped, because
 * profiling showed most real calls here ARE nested -- derivation.c's
 * argument processing calls asnix_force_for_print on a thunk whose own
 * body ends up back in derivation.c calling asnix_force_for_print again
 * on a nested value, routinely several levels deep. Caching one buffer
 * pair PER DEPTH instead (grown lazily, like eval_file.c's import
 * cache) is safe because depth is strictly LIFO: a slot is only ever
 * "in use" while some live frame at that exact depth hasn't returned
 * yet, and the asm side already enforces that discipline by construction
 * (push on entry, pop on exit) -- same precedent as every other
 * linear/array-indexed cache in this codebase, just indexed by depth
 * instead of scanned by key, since depth is already a dense small
 * integer with no need for a search.
 */
void *asnix_force_scratch_valstack(int64_t depth);
void *asnix_force_scratch_retstack(int64_t depth);

#endif /* __ASSEMBLER__ */

#endif /* ASNIX_FORCE_SCRATCH_H */
