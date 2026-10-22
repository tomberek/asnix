#ifndef ASNIX_BYTECODE_CACHE_H
#define ASNIX_BYTECODE_CACHE_H

#include "asnix/ir.h"

typedef struct Ast Ast; /* ast.h's own tag -- forward-declared so this header
                          * doesn't need a full #include "ast.h" */

/* On-disk cache of compiled IrPrograms, keyed by (resolved absolute file
 * path, source file content) -- both are part of the key, not just
 * content, because a compiled program can embed its own file PATH
 * directly as data (AST_CURPOS's `file` field, compiler.c) -- two
 * identical-content files at different paths must NOT share a cache
 * entry, or the second one's __curPos would silently report the
 * first's path. See bytecode.h/.c for the actual (de)serialization this
 * wraps, and docs/architecture.md's M92 notes for the full design
 * writeup (iets's cache.rs is the direct model for this strategy, not
 * for asnix's own bytecode shape -- see that doc for why the two
 * differ).
 *
 * Entirely opt-in: every function here is a no-op (always a cache miss,
 * never writes) unless the ASNIX_BYTECODE_CACHE=1 environment variable
 * is set. Any anomaly at any point (missing/unwritable directory,
 * truncated/corrupt file, bad magic, a deserialize failure) is treated
 * as a plain cache miss -- never fatal, never surfaced to the caller
 * beyond "use the normal compile path instead." */

/* ASNIX_BYTECODE_CACHE_STATS=1 (alongside ASNIX_BYTECODE_CACHE=1) prints
 * a one-line "N hits, M misses, K stores (P% hit rate)" summary to
 * stderr at process exit -- the cache is otherwise completely silent by
 * design (a miss, a corrupt entry, and "never looked up" are all
 * indistinguishable from the outside), so this is the only way to tell
 * whether a given run is actually benefiting from it. */

/* True iff ASNIX_BYTECODE_CACHE=1 is set. Checked once and cached. */
int asnix_bytecode_cache_enabled(void);

/* Looks up a cache entry for `resolved_path` (must be the already-
 * resolved absolute path eval_file.c compiles against) given the file's
 * raw `source`/`source_len` bytes (used only to compute the content
 * hash -- not retained). Returns a freshly deserialized IrProgram ready
 * for asnix_eval() on a hit, or NULL on a miss/disabled/any anomaly.
 * Never fatal. */
IrProgram *asnix_bytecode_cache_lookup(const char *resolved_path, const char *source, size_t source_len);

/* Writes `prog` to the cache under the same key asnix_bytecode_cache_lookup
 * would look it up with. No-op if the cache is disabled. Best-effort:
 * any failure (serialize refuses an unsupported construct, disk error,
 * lock contention) is silently ignored -- a missing/stale cache entry is
 * always safe, just slower, never wrong. */
void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, IrProgram *prog);

/* M92: opt-in background prefetch -- ASNIX_BYTECODE_PRODUCER=1 (on top
 * of ASNIX_BYTECODE_CACHE=1, which it requires) scans a freshly parsed
 * AST for statically-literal `import "<path>"`-shaped call targets not
 * already cached, and fork()s one child process per discovery to
 * compile-and-cache each in the background while the parent continues
 * evaluating. Deliberately a forked PROCESS, not a pthread: the global
 * arena (asnix_arena, src/asm/arena.S) and symbol table (symtab.c) have
 * no synchronization at all, and retrofitting that safely was judged
 * out of scope for this pass (see docs/architecture.md's M92 notes) --
 * fork()'s copy-on-write address-space duplication gives the same
 * "separate core, separate L1/L2 pointer-chasing, dedicated parser"
 * property the threading hypothesis wants, with zero risk of the parent
 * and child's compile-time allocators racing, since after fork() they
 * are simply two independent processes that happen to share no mutable
 * state at all. The child writes its result to the same on-disk cache
 * asnix_bytecode_cache_store would, then exits; the parent only ever
 * reads that cache through the ordinary asnix_bytecode_cache_lookup
 * path, so there is no cross-process pointer handoff of any kind.
 * No-op (does nothing, returns immediately) unless both env vars are
 * set. Safe to call on every parsed file; internally no-ops on anything
 * that isn't a plain, already-cached-checked, statically-resolvable
 * import target. */
void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir);

/* Waits for (reaps) any outstanding prefetch children this process
 * started whose target path matches `resolved_path` -- called right
 * before a synchronous cache lookup for that same path, so a producer
 * that's already most of the way done gets a chance to finish and
 * populate the cache before the consumer falls back to compiling it
 * itself redundantly. Bounded wait (a short timeout, not indefinite) --
 * if the child hasn't finished yet, returns immediately and the caller
 * proceeds with its own normal cache-miss fallback exactly as if no
 * prefetch had ever been attempted. No-op if prefetching is disabled or
 * no outstanding child matches. */
void asnix_bytecode_prefetch_wait(const char *resolved_path);

#endif /* ASNIX_BYTECODE_CACHE_H */
