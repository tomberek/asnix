INC          := -Iinclude -Isrc/c
CC           ?= cc

# Debug-by-default: fastest compile, best debugging. `make release` opts into -O2
# for bench/ runs only -- the hot path under measurement is hand-written asm,
# not compiler-optimized C, so the everyday loop shouldn't pay -O2 compile cost.
OPT          ?= -O0
# `=` (recursive), not `:=` -- CFLAGS must re-expand $(OPT) at each use so
# `release`'s target-specific `OPT := -O2` (below) actually takes effect.
# `:=` would bake in whatever OPT was at *this* line (-O0) permanently,
# silently making `make release` build an unoptimized binary.
CFLAGS       = -std=c11 -Wall -Wextra -g $(OPT) $(INC) -MMD -MP -U_FORTIFY_SOURCE -Wa,--noexecstack
LDFLAGS      :=

MAKEFLAGS    += -j$(shell nproc 2>/dev/null || echo 4)

ASM_SRCS     := $(wildcard src/asm/*.S src/asm/ops/*.S)
C_ALL_SRCS   := $(wildcard src/c/*.c)
C_LIB_SRCS   := $(filter-out src/c/main.c,$(C_ALL_SRCS))
MAIN_SRC     := src/c/main.c

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

.PHONY: all release test test-e2e test-conformance clean watch
all: asnix

release: OPT := -O2
release: asnix

asnix: $(MAIN_OBJ) libasnix.a
	$(CC) $(LDFLAGS) -o $@ $(MAIN_OBJ) -L. -lasnix

libasnix.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

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

watch:
	command -v entr >/dev/null || { echo "entr not found -- enter the flake devShell"; exit 1; }
	find src include tests -type f | entr -c $(MAKE) test

clean:
	rm -f asnix libasnix.a $(ALL_OBJS) $(DEPS) $(TEST_ASM_BINS) $(TEST_IR_BINS) $(TEST_C_BINS)
	rm -f $(TEST_ASM_SRCS:.c=.d) $(TEST_IR_SRCS:.c=.d) $(TEST_C_SRCS:.c=.d)

-include $(DEPS)
-include $(TEST_ASM_SRCS:.c=.d)
-include $(TEST_IR_SRCS:.c=.d)
-include $(TEST_C_SRCS:.c=.d)
