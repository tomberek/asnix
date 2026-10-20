{ stdenv, lib }:

stdenv.mkDerivation {
  pname = "asnix";
  version = "0-unstable";

  src = lib.cleanSource ../.;

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
