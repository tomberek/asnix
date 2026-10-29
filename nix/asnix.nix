{ stdenv, lib, makeWrapper, mimalloc, liburing, openssl }:

stdenv.mkDerivation {
  pname = "asnix";
  version = "0-unstable";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [ makeWrapper ];
  buildInputs = [ liburing ];

  # M146: static-link mimalloc into the final `asnix` binary as a
  # process-wide malloc/free/calloc/realloc override -- see Makefile's
  # MIMALLOC_LDLIBS and flake.nix's devShell shellHook (same variable,
  # same absolute-store-path convention) for the full rationale.
  ASNIX_MIMALLOC_LIB = "${mimalloc}/lib/libmimalloc.a";

  # Same handoff convention flake.nix's devShell already uses: liburing's
  # headers live in the `.dev` output, the shared lib in `.out` (see
  # Makefile's ASNIX_LIBURING_INC/ASNIX_LIBURING_LIBDIR, a hard build
  # dependency there, not optional like mimalloc above). The Makefile
  # link step bakes LIBDIR in as an rpath, so the built binary finds
  # liburing at runtime with no wrapper needed for this one.
  ASNIX_LIBURING_INC = "${liburing.dev}/include";
  ASNIX_LIBURING_LIBDIR = "${liburing.out}/lib";

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

  # hash_shim.c's builtins.hashString/derivation-hashing support dlopens
  # libcrypto by PATH STRING at runtime (getenv("ASNIX_LIBCRYPTO_LIB")),
  # unlike mimalloc/liburing above which are resolved at BUILD time and
  # end up statically linked or rpath-embedded in the binary itself --
  # a plain derivation attribute only reaches the build environment, not
  # whoever runs $out/bin/asnix later, so this needs an actual wrapper
  # (the standard nixpkgs pattern for a runtime-resolved env var), not
  # just another `ASNIX_*` attribute next to the two above.
  postFixup = ''
    wrapProgram $out/bin/asnix \
      --set ASNIX_LIBCRYPTO_LIB "${lib.getLib openssl}/lib/libcrypto${stdenv.hostPlatform.extensions.sharedLibrary}"
  '';
}
