#ifndef ASNIX_ATERM_H
#define ASNIX_ATERM_H

#include <stddef.h>

/*
 * ATerm (.drv) text serialization, scoped to exactly what M8's minimal
 * `derivation {...}` needs: zero input derivations, zero input sources
 * (see docs/architecture.md's M8 notes for why -- derivation-to-
 * derivation dependencies need Nix's string-context tracking, which this
 * project doesn't have yet). A later milestone generalized the original
 * single-fixed-"out"-output assumption to an arbitrary, caller-supplied
 * list of named outputs (see derivation.c), matching real Nix's own
 * `Outputs` section (~/nix/src/libstore/derivation/aterm.cc's
 * `unparse`), which is a map from output name to (path, hashAlgo, hash)
 * and is NOT folded into `env`.
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

/* One entry of the ATerm's `Outputs` section: `name` is the output name
 * (e.g. "out", "bin"), `path` is that output's store path (or "" for the
 * masked/self-referential-hash pass, see store_path.h). This project
 * only ever produces plain input-addressed outputs, so the other two
 * ATerm fields real Nix's format has per output (hashAlgo, hash -- used
 * for fixed-output/content-addressed derivations) are always written as
 * "" here, matching real Nix's own InputAddressed/Deferred encoding. */
typedef struct {
    const char *name;
    const char *path;
} AtermOutput;

/* Builds "Derive([(name,path,\"\",\"\")...],[],[],<system>,<builder>,
 * [args...],[(k,v)...])" with Nix's exact escaping (only \n \r \t \" \\
 * are ever escaped). `outputs` MUST already be sorted ascending by name
 * (same convention `env` already has) -- Nix's canonical ATerm form
 * requires both maps sorted for determinism and this function does not
 * re-sort either. `env` MUST already be sorted ascending by key.
 * Returns a malloc'd, NUL-terminated string (caller frees) -- this is
 * scratch/build-time text, not part of the runtime Value graph, so it
 * doesn't need the arena. */
char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs, const char *system,
                                    const char *builder, const char **args, size_t nargs, const AtermEnvVar *env,
                                    size_t nenv);

#endif /* ASNIX_ATERM_H */
