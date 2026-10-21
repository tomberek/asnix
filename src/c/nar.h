#ifndef ASNIX_NAR_H
#define ASNIX_NAR_H

#include <stddef.h>

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

#endif /* ASNIX_NAR_H */
