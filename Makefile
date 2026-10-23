INC          := -Iinclude -Isrc/c
CC           ?= cc

# -O2 by default: a background perf-focused agent measured a genuine ~2x
# wall-clock difference on a real nixpkgs workload between -O0 and -O2
# (`d.nix`, ffmpeg's closure: ~9.9s -> ~4.9s user) -- plenty of real work
# (derivation building, string/JSON/ATerm encoding, hashing) lives in
# compiled C, not just the hand-written asm dispatch loop, so the old
# "the hot path is asm, C doesn't matter" assumption doesn't hold for
# this project's actual real-world workloads. `make debug` opts back
# into -O0 for the fastest compile / most gdb-friendly build when
# actively debugging (this session's own memory-leak investigation
# relied on breakpoint-based debugging, where -O0's lack of
# optimization-driven variable elision/reordering genuinely helps).
# `release` is kept as a now-redundant alias (identical to the default)
# so any existing script/doc referencing `make release` (e.g. bench/run)
# keeps working unchanged.
OPT          ?= -O2
# `=` (recursive), not `:=` -- CFLAGS must re-expand $(OPT) at each use so
# `debug`'s target-specific `OPT := -O0` (below) actually takes effect.
# `:=` would bake in whatever OPT was at *this* line permanently.
CFLAGS       = -std=c11 -Wall -Wextra -g $(OPT) $(INC) -MMD -MP -U_FORTIFY_SOURCE -Wa,--noexecstack
LDFLAGS      :=

# M146: statically link mimalloc into the final `asnix` executable as a
# process-wide override of malloc/free/calloc/realloc, IF the flake's
# devShell (or nix/asnix.nix's packaged build) set ASNIX_MIMALLOC_LIB to
# mimalloc's own libmimalloc.a (same env-var-handoff convention as
# ASNIX_LIBCRYPTO_LIB, see flake.nix's shellHook) -- gracefully absent,
# not a hard build failure, for anyone running plain `cc`/`make` outside
# the flake's devShell. mimalloc's own static archive already exports
# standard-named `malloc`/`free`/`calloc`/`realloc` symbols directly (its
# default Unix build has override baked in -- confirmed directly via
# `nm` before relying on it), so simply linking it into the executable's
# OWN symbol table is sufficient for a full override: no header include,
# no source change, no `mimalloc.h`/`mi_malloc` call site anywhere in
# this tree. Scoped to the `asnix` binary ONLY (not `irdump`, not the
# tests/{asm,ir,c}/* unit-test binaries) -- this is a real-eval-workload
# speed change, not something the correctness-focused test binaries need.
# Measured (interleaved, `perf stat -e cpu_core/instructions/,cpu_core/
# cycles/`, `taskset -c 0`, 10 alternating trials, real `d.nix`/ffmpeg
# eval): instructions 8,031.9M -> 7,317.8M (-8.9%), consistent in every
# single one of 10 trials to within 0.03%; cycles similarly down (noisier,
# as always on this shared machine, but one-directional). See
# docs/architecture.md's M146 section for the full writeup.
ifdef ASNIX_MIMALLOC_LIB
MIMALLOC_LDLIBS := $(ASNIX_MIMALLOC_LIB) -lpthread
endif

MAKEFLAGS    += -j$(shell nproc 2>/dev/null || echo 4)

ASM_SRCS     := $(wildcard src/asm/*.S src/asm/ops/*.S)
C_ALL_SRCS   := $(wildcard src/c/*.c)
C_LIB_SRCS   := $(filter-out src/c/main.c src/c/irdump.c,$(C_ALL_SRCS))
MAIN_SRC     := src/c/main.c
IRDUMP_SRC   := src/c/irdump.c

LIB_OBJS     := $(ASM_SRCS:.S=.o) $(C_LIB_SRCS:.c=.o)
MAIN_OBJ     := $(MAIN_SRC:.c=.o)
ALL_OBJS     := $(LIB_OBJS) $(MAIN_OBJ)
DEPS         := $(ALL_OBJS:.o=.d)

TEST_ASM_SRCS := $(wildcard tests/asm/*.c)
TEST_ASM_BINS := $(TEST_ASM_SRCS:.c=)

TEST_IR_SRCS  := $(wildcard tests/ir/*.c)
TEST_IR_BINS  := $(TEST_IR_SRCS:.c=)

TEST_C_SRCS   := $(wildcard tests/c/*.c)
TEST_C_BINS   := $(TEST_C_SRCS:.c=)

E2E_NIX         := $(wildcard tests/e2e/*.nix)
CONFORMANCE_NIX := $(wildcard tests/conformance/*.nix)

# M125: `tests/flake/<name>/` fixtures for `--flake`, one directory per
# flake under test (each holds its own `flake.nix`) with any number of
# `*.attr`/`*.expected` sidecar pairs (the attrpath fragment to select,
# and the expected printed output of selecting it) -- see test-flake's
# own recipe below.
FLAKE_DIRS := $(wildcard tests/flake/*)

# Stale-optimization-flag guard: `make`/`make debug`/`make release` all
# share the same .o files but compile them with different $(OPT) values,
# and `-MMD -MP`'s header-dependency tracking has no way to know the
# *flags* changed (only the *sources* did) -- switching between them
# without an explicit `make clean` silently leaves a stale mix of
# differently-optimized objects (found this session: a background agent
# measured a confusing ~2x "slowdown" that turned out to be exactly
# this, not a real regression). Checked once here, at parse time --
# before any recipe/job dispatch, so this can't race with the parallel
# (`-j`) object-compile rules below -- against whichever OPT value this
# invocation will actually use (only `debug` currently diverges from
# the global default; `$(MAKECMDGOALS)` is still correct even for a
# bare `make`/`make all`, since an empty filter result just falls
# through to $(OPT)).
ifneq ($(filter debug,$(MAKECMDGOALS)),)
EFFECTIVE_OPT := -O0
else
EFFECTIVE_OPT := $(OPT)
endif
# M146: folded into the same stamp key as EFFECTIVE_OPT above (not a
# separate stamp file) -- MIMALLOC_LDLIBS doesn't affect any .o's own
# compile flags, only the final `asnix` link step, but the stamp's own
# `rm -f ... asnix ...` already forces a relink on ANY key change, which
# is exactly what's needed here too: switching in or out of a devShell
# that sets ASNIX_MIMALLOC_LIB, without an explicit `make clean`, must
# not silently keep a stale link of `asnix` from the other configuration.
OPT_STAMP := .opt-stamp
STAMP_KEY := $(EFFECTIVE_OPT)|$(MIMALLOC_LDLIBS)
$(shell last=$$(cat $(OPT_STAMP) 2>/dev/null); \
	if [ "$$last" != "$(STAMP_KEY)" ]; then \
		rm -f $(ALL_OBJS) $(DEPS) asnix libasnix.a $(TEST_ASM_BINS) $(TEST_IR_BINS) $(TEST_C_BINS) irdump $(IRDUMP_SRC:.c=.o); \
		echo "$(STAMP_KEY)" > $(OPT_STAMP); \
	fi)

.PHONY: all release debug test test-e2e test-conformance test-flake test-nix-conformance test-nixpkgs-hash clean watch
all: asnix

release: OPT := -O2
release: asnix

debug: OPT := -O0
debug: asnix

asnix: $(MAIN_OBJ) libasnix.a
	$(CC) $(LDFLAGS) -o $@ $(MAIN_OBJ) -L. -lasnix $(MIMALLOC_LDLIBS)

libasnix.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

# Companion tool: decodes+pretty-prints a compiled IrProgram (see
# irdump.c's own header comment) -- not part of `all`/`make`'s default
# build, built on demand like the test binaries below.
irdump: $(IRDUMP_SRC:.c=.o) libasnix.a
	$(CC) $(LDFLAGS) -o $@ $(IRDUMP_SRC:.c=.o) -L. -lasnix

# .S files are cpp-preprocessed by `cc` (capital S => real #include of
# include/asnix/*.h, shared verbatim with the C side -- zero constant drift).
%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Tier-1 ("smoke") tests: in-process asm-primitive unit tests + M3 IR fixtures.
# No subprocess spawn beyond the test binary itself; target is sub-second total.
# `make test TEST=test_value` builds+runs just that one binary (matched by
# basename) -- handy when iterating on a single area; bare `make test` still
# runs the full tier-1 set.
ALL_TEST_BINS := $(TEST_ASM_BINS) $(TEST_IR_BINS) $(TEST_C_BINS)
ifeq ($(TEST),)
RUN_TEST_BINS := $(ALL_TEST_BINS)
else
RUN_TEST_BINS := $(filter %/$(TEST),$(ALL_TEST_BINS))
endif

test: $(RUN_TEST_BINS)
	@if [ -n "$(TEST)" ] && [ -z "$(RUN_TEST_BINS)" ]; then \
		echo "no test binary matches TEST=$(TEST) (known: $(notdir $(ALL_TEST_BINS)))" >&2; exit 1; \
	fi
	@set -e; for t in $(RUN_TEST_BINS); do echo "== $$t =="; ./$$t; done

tests/asm/%: tests/asm/%.c libasnix.a
	$(CC) $(CFLAGS) -o $@ $< -L. -lasnix

tests/ir/%: tests/ir/%.c libasnix.a
	$(CC) $(CFLAGS) -o $@ $< -L. -lasnix

tests/c/%: tests/c/%.c libasnix.a
	$(CC) $(CFLAGS) -o $@ $< -L. -lasnix

# Tier-2: real .nix source through the full lexer/parser/compiler/evaluator,
# diffed against a .expected sidecar. Not run by default `make test` --
# subprocess-per-case is slower than tier 1's in-process checks.
test-e2e: asnix
	@set -e; for f in $(E2E_NIX); do \
		exp="$${f%.nix}.expected"; \
		got=$$(./asnix "$$f"); \
		want=$$(cat "$$exp"); \
		if [ "$$got" = "$$want" ]; then echo "== $$f == OK"; \
		else echo "== $$f == FAIL (got '$$got', want '$$want')"; exit 1; fi; \
	done

# Tier-3: cases mined from numtide/nix-conformance (see tests/conformance/
# README.md) -- same mechanics as test-e2e, separate target since these
# check against an external reference's expectations, not this project's
# own fixtures.
test-conformance: asnix
	@set -e; for f in $(CONFORMANCE_NIX); do \
		exp="$${f%.nix}.expected"; \
		got=$$(./asnix "$$f"); \
		want=$$(cat "$$exp"); \
		if [ "$$got" = "$$want" ]; then echo "== $$f == OK"; \
		else echo "== $$f == FAIL (got '$$got', want '$$want')"; exit 1; fi; \
	done

# M125: `--flake DIR#attr.path` fixtures -- same mechanics as test-e2e/
# test-conformance (compare printed stdout against a sidecar), except the
# invocation is `./asnix --flake DIR#attr` rather than `./asnix FILE`, so
# this needed its own small loop instead of reusing E2E_NIX's. Sidecars
# (`*.attr`/`*.expected`, holding the attrpath fragment to select and its
# expected printed output) live NEXT TO each flake directory, named
# `<dir>.<case>.attr`/`.expected` -- deliberately NOT inside the flake
# directory itself, since `self.outPath`'s NAR hash covers every file the
# directory contains; a sidecar living inside it would make its own
# existence change the very value it's testing.
test-flake: asnix
	@set -e; for d in $(FLAKE_DIRS); do \
		for attrfile in $$d.*.attr; do \
			[ -f "$$attrfile" ] || continue; \
			attr=$$(cat "$$attrfile"); \
			exp="$${attrfile%.attr}.expected"; \
			got=$$(./asnix --flake "$$d#$$attr"); \
			want=$$(cat "$$exp"); \
			if [ "$$got" = "$$want" ]; then echo "== $$d#$$attr == OK"; \
			else echo "== $$d#$$attr == FAIL (got '$$got', want '$$want')"; exit 1; fi; \
		done; \
	done

# Tier-4: sweeps the FULL numtide/nix-conformance corpus (`lang/eval-okay-
# *.nix`/`eval-fail-*.nix`, ~800 files), not just the handful of cases
# mined into tests/conformance/ above. This is an external, un-checked-in
# dependency -- clone https://github.com/numtide/nix-conformance to
# NIX_CONFORMANCE_DIR first (default /tmp/nix-conformance), or pass
# NIX_CONFORMANCE_DIR=/path/to/it on the command line. Does NOT `exit 1`
# on failure (unlike test-e2e/test-conformance above) -- this corpus has
# known, documented gaps (see docs/architecture.md's M5x scope-notes
# sections), so a nonzero fail count is the expected steady state, not a
# build-breaking regression; this target's job is to report the count and
# the failing-file list, not to gate the build.
#
# A `.flags` sidecar marks a case needing CLI flags/features this target
# doesn't attempt to pass (counted as skipped, not failed). `eval-okay-*`
# cases compare stdout against the `.exp` sidecar exactly, same as
# test-e2e/test-conformance. `eval-fail-*` cases only check for a nonzero
# exit (not the exact error text, matching this project's standing
# convention of matching an error's *class*, not its wording) -- a crash
# (signal, e.g. a segfault) also counts as "pass" here, same coincidental-
# pass caveat already documented for a few mined fixtures in
# tests/conformance/, not a true clean-error pass.
# M64: two env/comparison details, both matching numtide/nix-conformance's
# own documented adapter protocol (README.md's "Writing an adapter"
# section) exactly, rather than inventing a separate convention here:
#   - HOME=/fake-home is one of the fixed env vars the protocol says every
#     adapter is invoked with -- needed for `~`-prefixed path literals
#     (eval-okay-home-path-not-normalised.nix, eval-okay-path-string-
#     interpolation.nix's "home" field); no other currently-passing case
#     in this corpus references $HOME at all (grepped), so this can only
#     ever change a currently-FAILING comparison, never break a passing one.
#   - The repo root -> "/pwd" substitution mirrors the corpus's own `run`
#     script (`printf '%s\n' "${out//$root//pwd}"`) -- several `.exp` files
#     (e.g. eval-okay-path-string-interpolation.exp, eval-okay-short-path-
#     literal-warn.exp) bake in that placeholder for a relative path
#     literal's resolved absolute form, which this target previously had
#     no way to match (see docs/architecture.md's old "not mechanically
#     comparable yet" note on those cases -- superseded by this). Safe for
#     the same reason as above: a `.exp` file can never itself contain this
#     clone's literal local path, so this substitution is a no-op on every
#     comparison that wasn't already going to fail.
#
# M67: `TEST_VAR=foo` -- the real numtide/nix-conformance `run` script
# exports this (plus `NIX_PATH`/`HOME`, not replicated here -- nothing in
# this sweep needs them yet) before running any case (its own
# `run:173`/README.md:84); eval-okay-getenv.nix reads it. Without this,
# that one test's `.exp` ("foobar") can never match either implementation
# -- confirmed directly: real `nix-instantiate --eval` on that same file,
# run without TEST_VAR set, also gives a different result ("bar") -- an
# environment-setup artifact of this lightweight sweep script, not an
# asnix bug.
NIX_CONFORMANCE_DIR ?= /tmp/nix-conformance
test-nix-conformance: asnix
	@dir="$(NIX_CONFORMANCE_DIR)/lang"; \
	bin="$(CURDIR)/asnix"; \
	if [ ! -d "$$dir" ]; then \
		echo "test-nix-conformance: '$$dir' not found -- clone" \
		     "https://github.com/numtide/nix-conformance to $(NIX_CONFORMANCE_DIR)" \
		     "(or pass NIX_CONFORMANCE_DIR=/path/to/it) first" >&2; \
		exit 1; \
	fi; \
	export HOME=/fake-home; \
	root="$(NIX_CONFORMANCE_DIR)"; \
	pass=0; fail=0; skip=0; failed=""; \
	cd "$$dir" && \
	export TEST_VAR=foo; \
	for f in eval-okay-*.nix; do \
		base="$${f%.nix}"; \
		if [ -f "$$base.flags" ]; then skip=$$((skip+1)); continue; fi; \
		exp="$$base.exp"; \
		if [ ! -f "$$exp" ]; then skip=$$((skip+1)); continue; fi; \
		got=$$(timeout 10 "$$bin" "$$f" 2>/dev/null); \
		got="$${got//$$root//pwd}"; \
		want=$$(cat "$$exp"); \
		if [ "$$got" = "$$want" ]; then pass=$$((pass+1)); \
		else fail=$$((fail+1)); failed="$$failed $$f"; fi; \
	done; \
	for f in eval-fail-*.nix; do \
		base="$${f%.nix}"; \
		if [ -f "$$base.flags" ]; then skip=$$((skip+1)); continue; fi; \
		timeout 10 "$$bin" "$$f" >/dev/null 2>/dev/null; rc=$$?; \
		if [ "$$rc" -ne 0 ] && [ "$$rc" -ne 124 ]; then pass=$$((pass+1)); \
		else fail=$$((fail+1)); failed="$$failed $$f"; fi; \
	done; \
	echo "pass=$$pass fail=$$fail skip=$$skip"; \
	if [ -n "$$failed" ]; then \
		echo "failed:"; \
		for f in $$failed; do echo "  $$f"; done; \
	fi

# Real-nixpkgs derivation-hash correctness sweep (added post-M76, see
# tests/nixpkgs-hash/attrs.txt's own header comment for why): every
# attribute path listed there gets its `.outPath`/`.drvPath` checked
# byte-for-byte against real `nix-instantiate --eval --strict` --
# non-gating, like test-nix-conformance above, since this corpus is
# expected to have real failures for a long time (the standing d.nix
# bug-chase's own open-ended goal, full nixpkgs bootstrap-chain hash
# parity) and a hard-fail-the-build gate would make this target useless
# the moment ANY package's upstream dependency doesn't fully match yet.
# Both a crash (nonzero asnix exit) and a hash MISMATCH count as
# failures, reported distinguished (`(crash)`/`(mismatch)`) -- both are
# real bugs, just different symptoms; a real-Nix eval failure for a
# listed attribute (typo, doesn't exist) is silently skipped rather than
# counted either way, matching test-nix-conformance's own `.flags`-style
# "we can't judge this one" convention.
test-nixpkgs-hash: asnix
	@pkgsfile="tests/nixpkgs-hash/pkgs.nix"; \
	bin="$(CURDIR)/asnix"; \
	pass=0; fail=0; failed=""; \
	while IFS= read -r attr || [ -n "$$attr" ]; do \
		case "$$attr" in ""|\#*) continue;; esac; \
		for field in outPath drvPath; do \
			path="$$attr.$$field"; \
			want=$$(timeout 120 nix-instantiate --eval --strict -A "$$path" "$$pkgsfile" 2>/dev/null); \
			[ $$? -ne 0 ] && continue; \
			got=$$(timeout 120 "$$bin" -A "$$path" "$$pkgsfile" 2>/dev/null); rc=$$?; \
			if [ "$$rc" -eq 0 ] && [ "$$got" = "$$want" ]; then pass=$$((pass+1)); \
			elif [ "$$rc" -ne 0 ]; then fail=$$((fail+1)); failed="$$failed $$path(crash)"; \
			else fail=$$((fail+1)); failed="$$failed $$path(mismatch)"; fi; \
		done; \
	done < tests/nixpkgs-hash/attrs.txt; \
	echo "pass=$$pass fail=$$fail"; \
	if [ -n "$$failed" ]; then \
		echo "failed:"; \
		for f in $$failed; do echo "  $$f"; done; \
	fi

watch:
	command -v entr >/dev/null || { echo "entr not found -- enter the flake devShell"; exit 1; }
	find src include tests -type f | entr -c $(MAKE) test

clean:
	rm -f asnix libasnix.a $(ALL_OBJS) $(DEPS) $(TEST_ASM_BINS) $(TEST_IR_BINS) $(TEST_C_BINS)
	rm -f $(TEST_ASM_SRCS:.c=.d) $(TEST_IR_SRCS:.c=.d) $(TEST_C_SRCS:.c=.d)
	rm -f $(OPT_STAMP)
	rm -f irdump $(IRDUMP_SRC:.c=.o) $(IRDUMP_SRC:.c=.d)

-include $(DEPS)
-include $(TEST_ASM_SRCS:.c=.d)
-include $(TEST_IR_SRCS:.c=.d)
-include $(TEST_C_SRCS:.c=.d)
-include $(IRDUMP_SRC:.c=.d)
