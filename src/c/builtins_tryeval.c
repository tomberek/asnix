/*
 * builtins.tryEval expr -- M30. Catches exactly what real Nix's own
 * `tryEval` catches: `throw` and a failed `assert` (both the single
 * `AssertionError` exception class cppnix uses, confirmed in its
 * `primops.cc`), nothing else -- see tryeval.h's full comment for the
 * mechanism (setjmp/longjmp) and its one accepted limitation.
 *
 * Verified directly against `nix-instantiate --eval` before writing any
 * code:
 *   tryEval (throw "x")       == { success = false; value = false; }
 *   tryEval (assert false; 1) == { success = false; value = false; }
 *   tryEval 42                == { success = true; value = 42; }
 *   tryEval { a = throw "x"; } == { success = true; value = { a = <CODE>; } }
 *     (shallow: only forces to WHNF, never recurses into attrset/list
 *     values -- the nested `throw` is never reached)
 *   tryEval (abort "x")       -- NOT caught, the whole process still
 *     aborts unconditionally (abort is not an AssertionError in real Nix)
 *   tryEval (1 + "a")         -- NOT caught either (a plain type error,
 *     not an AssertionError)
 */
#include "builtins_tryeval.h"

#include <setjmp.h>
#include <stdint.h>

#include "asnix/ir.h" /* asnix_force_for_print */
#include "builtins_shared.h"
#include "symtab.h"
#include "tryeval.h"

/* `asnix_force_for_print` (force.S) saves/restores these two globals
 * around its own nested valstack/retstack buffer (see stacklimits.S's
 * header comment) -- but only via its own ORDINARY epilogue, which a
 * `longjmp` triggered from deep inside that nested evaluation (a
 * caught `throw`/`assert`) skips entirely. `setjmp`/`longjmp`
 * themselves correctly restore every genuine CPU register (%r12/%r14
 * included, both SysV callee-saved) back to their values at this
 * function's own `setjmp` call -- but these two are plain C global
 * variables, not registers, so they need their own explicit save/
 * restore here, on both the normal-return and the caught-exception
 * path, or the very next VALSTACK/RETSP push after a catch compares
 * against a stale limit left over from the abandoned nested force and
 * aborts with a bogus "stack overflow" (found directly: `tryEval (throw
 * "x")` hit exactly this before adding the explicit restore below). */
extern int64_t asnix_valstack_limit;
extern int64_t asnix_retsp_limit;

static Value prim_tryEval(const Value *args, int64_t argc) {
    (void)argc;
    int64_t saved_valstack_limit = asnix_valstack_limit;
    int64_t saved_retsp_limit = asnix_retsp_limit;

    jmp_buf buf;
    asnix_tryeval_push(&buf);

    Value value;
    int success;
    if (setjmp(buf) == 0) {
        value = asnix_force_for_print(args[0]);
        success = 1;
    } else {
        value = asnix_make_imm(ASNIX_IMM_FALSE);
        success = 0;
    }
    asnix_tryeval_pop();
    asnix_valstack_limit = saved_valstack_limit;
    asnix_retsp_limit = saved_retsp_limit;

    uint32_t syms[2];
    Value vals[2];
    syms[0] = asnix_intern("success", 7);
    vals[0] = asnix_make_imm(success ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
    syms[1] = asnix_intern("value", 5);
    vals[1] = value;
    return asnix_bshared_make_attrs(syms, vals, 2);
}

static Value build_tryEval(void) { return asnix_bshared_make_primop_noforce((void *)prim_tryEval, 1); }

const AsnixBuiltinMiscDef ASNIX_TRYEVAL_BUILTINS[] = {
    { "tryEval", build_tryEval },
};

const size_t ASNIX_TRYEVAL_BUILTINS_COUNT = sizeof(ASNIX_TRYEVAL_BUILTINS) / sizeof(ASNIX_TRYEVAL_BUILTINS[0]);
