#ifndef ASNIX_ATERM_H
#define ASNIX_ATERM_H

#include <stddef.h>

/*
 * ATerm (.drv) text serialization, scoped to exactly what M8's minimal
 * `derivation {...}` needs: a single output named "out", zero input
 * derivations, zero input sources (see docs/architecture.md's M8 notes
 * for why -- derivation-to-derivation dependencies need Nix's string-
 * context tracking, which this project doesn't have yet).
 *
 * Format and escaping rules ported from ~/nix/src/libstore/derivation/
 * aterm.cc and verified byte-for-byte against two real references before
 * writing this: numtide/nix-conformance's
 * drv/upstream/ia-advanced-attributes-defaults.drv, and a real
 * `nix-instantiate -E 'derivation { name = "asnix-test"; ... }'` run
 * locally -- both reproduced exactly by a throwaway Python port of this
 * same algorithm before any C was written.
 */

typedef struct {
    const char *key;
    const char *value;
} AtermEnvVar;

/* Builds "Derive([(\"out\",<out_path>,\"\",\"\")],[],[],<system>,<builder>,
 * [args...],[(k,v)...])" with Nix's exact escaping (only \n \r \t \" \\
 * are ever escaped). `out_path` may be "" (for the masked/self-
 * referential-hash pass, see store_path.h) or a real store path (the
 * final pass). `env` MUST already be sorted ascending by key -- Nix's
 * canonical ATerm form requires it for determinism and this function
 * does not re-sort. Returns a malloc'd, NUL-terminated string (caller
 * frees) -- this is scratch/build-time text, not part of the runtime
 * Value graph, so it doesn't need the arena. */
char *asnix_build_derivation_aterm(const char *out_path, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv);

#endif /* ASNIX_ATERM_H */
