#ifndef ASNIX_NAR_H
#define ASNIX_NAR_H

#include <stddef.h>
#include <stdint.h>

/*
 * M75: copies a local filesystem path (file, directory, or symlink)
 * into the content-addressed store the same way real Nix's own
 * `addToStore` (method=NixArchive/"recursive", hashAlgo=sha256) does:
 * serialize it as a NAR ("nix-archive-1", ~/nix/src/libutil/archive.cc,
 * format verified byte-for-byte against that source -- string fields
 * are an 8-byte little-endian length, the raw bytes, then zero-padded
 * to the next 8-byte boundary; directory entries sorted ascending by
 * name), SHA-256 that, then feed the digest through the exact same
 * `makeStorePath("source", digest, name)` formula M73's
 * `compute_fixed_output_path` already uses for its sha256+recursive
 * branch (store_path.h's `asnix_make_store_path_from_digest`, via the
 * usual preimage-then-outer-hash pipeline).
 *
 * Found needed chasing `d.nix`: real Nix's `hello.drv` embeds
 * nixpkgs's own `pkgs/stdenv/generic/source-stdenv.sh`/
 * `default-builder.sh` this exact way (a plain Path literal, coerced
 * to a string during derivation-attribute/string-interpolation
 * processing, copies the referenced file into the store and
 * substitutes the resulting store path) -- `asnix` previously just
 * printed the literal filesystem path verbatim, with no store-copy
 * and no `inputSrcs` tracking at all (a previously-documented, known
 * gap, see value_util.c's `coerce_to_string_for_interp`).
 *
 * Does NOT perform real Nix's actual filesystem side effect (writing
 * the content into `/nix/store/...`) -- same stance this project
 * already takes for `derivation`/`toFile` (no real store, no actual
 * build ever happens here): only the resulting path *string* is
 * computed, matching what every reachable use in this project's own
 * evaluation needs. Memoized by literal input path (same linear-scan-
 * array convention as M65's import cache) so hashing the same local
 * path twice within one process (e.g. once via `asnix_collect_input_refs`,
 * once via the coercion that follows it) doesn't re-walk/re-hash it.
 *
 * Fatal (prints a message and exits), matching this project's
 * standing convention, if `path` doesn't exist or names an
 * unsupported file type (anything other than a regular file,
 * directory, or symlink) -- real Nix's own `dumpPath` does the same.
 * Writes the NUL-terminated resulting store path into `out` (capacity
 * `out_cap`).
 */
void asnix_copy_path_to_store(const char *path, char *out, size_t out_cap);

/* M80: per-entry predicate for `asnix_copy_path_to_store_ext` below --
 * `builtins.path`'s `filter`/`builtins.filterSource`'s own callback,
 * one layer removed from any actual Nix `Value`/`asnix_apply` (this
 * module has never depended on the evaluator's value representation at
 * all, and this keeps it that way): the CALLER (builtins_path.c)
 * supplies a closure over the real Nix filter function, this module
 * just calls it with the entry's full filesystem path and real Nix's
 * own type-name string (`"regular"`/`"directory"`/`"symlink"`/
 * `"unknown"`). Returning 0 excludes the entry (and, for a directory,
 * its entire subtree -- it's simply never recursed into) from the NAR
 * exactly like real Nix's own `PathFilter`. */
typedef int (*AsnixNarFilter)(void *ctx, const char *path, const char *type);

/* M80: `builtins.path`/`builtins.filterSource`'s general form --
 * `asnix_copy_path_to_store` above is the single common case (no
 * filter, NAR/"recursive" method, name = basename, no pinned hash) this
 * generalizes: a custom `name` (NUL-terminated; NULL means "use
 * `path`'s own basename", matching `filterSource`/`builtins.path`'s
 * `name`-attribute-omitted default), an optional `filter` callback (NULL
 * for none), `recursive` selects the NixArchive (NAR, like
 * `asnix_copy_path_to_store`) vs. Flat (bare file content, `path` MUST
 * be a regular file) content-addressing method -- both ultimately go
 * through M73's own `asnix_compute_fixed_output_path` formula
 * (derivation.h), the exact same one a fixed-output derivation's own
 * `outputHash` uses, not a separately-derived one -- and an optional
 * `expected_sha256` (NULL for none): if given, the path's OWN computed
 * content hash must match it exactly (fatal otherwise), matching real
 * Nix's `builtins.path { ...; sha256 = ...; }` pinned-hash check.
 * Unlike `asnix_copy_path_to_store`, never memoized (the result depends
 * on `name`/`recursive`/`filter` too, not just `path`) -- fine, nothing
 * reachable calls this at `asnix_copy_path_to_store`'s own hot-path
 * volume. */
void asnix_copy_path_to_store_ext(const char *path, const char *name, int recursive, AsnixNarFilter filter,
                                   void *filter_ctx, const uint8_t expected_sha256[32], char *out, size_t out_cap);

#endif /* ASNIX_NAR_H */
