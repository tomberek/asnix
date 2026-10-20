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
            entr         # optional `make watch` loop
          ];
          shellHook = ''
            export ASNIX_ROOT=$(pwd)
          '';
        };
      });

      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/asnix.nix { };
      });
    };
}
