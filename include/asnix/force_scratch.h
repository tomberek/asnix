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

/*
 * src/asm/apply.S's asnix_apply (the re-entrant "call this Value from C"
 * entry point `map`/`filter`/`foldl'`/etc. use) was modeled on
 * asnix_force_for_print's own "own fresh VALSTACK+RETSP via
 * asnix_arena_alloc" pattern -- including the exact same per-call-fresh-
 * allocation inefficiency this file's own functions above were added to
 * fix, just never updated to match once that fix landed. Found chasing
 * a memory-usage question (asnix's own `asnix_arena.total_allocated`
 * read 4.4 GB evaluating d.nix against only ~227 MB of actual resident
 * memory -- the gap is virtual-address-space waste from unused mmap'd
 * pages, harmless on its own, but `asnix_apply` turned out to be BY FAR
 * the larger of the two culprits: it's reached far more often than
 * force_for_print, via every `map`/`filter`/`foldl'`-style builtin call
 * in a real nixpkgs evaluation). Separate depth counter/cache from the
 * force_for_print one above -- apply.S's own header comment is explicit
 * that this context is never shared with an enclosing force_for_print
 * (or eval, or another apply) invocation, so keeping the two caches
 * independent matches that existing design intent rather than
 * introducing new coupling between them. Same buffer sizes
 * (ASNIX_FORCE_VALSTACK_BYTES/ASNIX_FORCE_RETSTACK_BYTES above happen to
 * equal apply.S's own ASNIX_APPLY_VALSTACK_BYTES/ASNIX_APPLY_RETSTACK_BYTES
 * equ constants -- reused here rather than duplicating the same two
 * numbers under a second name).
 */
void *asnix_apply_scratch_valstack(int64_t depth);
void *asnix_apply_scratch_retstack(int64_t depth);

#endif /* __ASSEMBLER__ */

/*
 * src/asm/dispatch.S's own asnix_eval -- the main entry point, called
 * fresh for EVERY file import (eval_file.c's asnix_eval_file et al.),
 * which can nest (`import` inside an imported file) -- had the exact
 * same per-call-fresh-allocation pattern as force_for_print/apply
 * above, at 10x the size (8 MiB + 2 MiB vs. 256 KiB + 64 KiB each): a
 * full nixpkgs evaluation imports hundreds of distinct files, so this
 * turned out to be the single largest contributor to asnix_arena's
 * total_allocated figure, well above apply.S's own (already-fixed)
 * share.
 */
#define ASNIX_EVAL_VALSTACK_BYTES 8388608 /* 1,048,576 Values, 8 MiB -- must match dispatch.S's own equ */
#define ASNIX_EVAL_RETSTACK_BYTES 2097152 /* 65,536 call frames, 2 MiB -- must match dispatch.S's own equ */

#ifndef __ASSEMBLER__
void *asnix_eval_scratch_valstack(int64_t depth);
void *asnix_eval_scratch_retstack(int64_t depth);
#endif /* __ASSEMBLER__ */

#endif /* ASNIX_FORCE_SCRATCH_H */
