{
  description = "asnix: a from-scratch, hand-written x86-64 asm Nix evaluator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # asm is arch-specific by construction; see docs/architecture.md Risks.
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            binutils    # as, ld -- explicit, don't rely on ambient PATH
            gcc
            gdb
            valgrind
            openssl     # dlopen target for hash shims (M8)
            mimalloc    # static-link-time allocator override (M146)
            entr         # optional `make watch` loop
          ];
          shellHook = ''
            export ASNIX_ROOT=$(pwd)
            # hash_shim.c's dlopen target for builtins.hashString (M8):
            # nixpkgs' openssl has no single predictable store path, so
            # resolve it here and hand the shim the exact path via getenv
            # -- same NIXGOOSE_LIBBLAKE3_LIB-style convention nixgoose's
            # own flake.nix uses for its own not-a-system-package dlopen
            # targets (~/gnix/flake.nix).
            export ASNIX_LIBCRYPTO_LIB="$(ls ${pkgs.openssl.out}/lib/libcrypto.so* | head -1)"
            # M146: mimalloc's static archive, statically linked into the
            # final `asnix` binary ONLY (see Makefile's MIMALLOC_LDLIBS) --
            # a real, measured ~9% whole-program instruction-count win on
            # the real d.nix/ffmpeg eval (glibc ptmalloc's own malloc-
            # family overhead was asnix's single largest named instruction-
            # gap category vs. iets's mimalloc, per this milestone's own
            # writeup in docs/architecture.md). Absolute nix-store path
            # baked in at flake-eval time, not resolved by getenv at
            # runtime like ASNIX_LIBCRYPTO_LIB above -- this is a build-time
            # link input, not a dlopen target.
            export ASNIX_MIMALLOC_LIB="${pkgs.mimalloc}/lib/libmimalloc.a"
          '';
        };
      });

      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/asnix.nix { };
      });
    };
}
