{ stdenv, lib, mimalloc }:

stdenv.mkDerivation {
  pname = "asnix";
  version = "0-unstable";

  src = lib.cleanSource ../.;

  # M146: static-link mimalloc into the final `asnix` binary as a
  # process-wide malloc/free/calloc/realloc override -- see Makefile's
  # MIMALLOC_LDLIBS and flake.nix's devShell shellHook (same variable,
  # same absolute-store-path convention) for the full rationale.
  ASNIX_MIMALLOC_LIB = "${mimalloc}/lib/libmimalloc.a";

  buildPhase = ''
    runHook preBuild
    make -j$NIX_BUILD_CORES release
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp asnix $out/bin/asnix
    runHook postInstall
  '';
}
