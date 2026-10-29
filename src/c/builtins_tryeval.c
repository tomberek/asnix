#include "builtins_tryeval.h"

#include <setjmp.h>
#include <stdint.h>

#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "tryeval.h"

extern int64_t asnix_valstack_limit;
extern int64_t asnix_retsp_limit;

extern int64_t asnix_force_depth;

static Value prim_tryEval(const Value *args, int64_t argc) {
    (void)argc;
    int64_t saved_valstack_limit = asnix_valstack_limit;
    int64_t saved_retsp_limit = asnix_retsp_limit;
    int64_t saved_force_depth = asnix_force_depth;
    int64_t saved_inprogress_head = asnix_tryeval_save_inprogress_head();

    jmp_buf buf;
    asnix_tryeval_push(&buf);

    Value value;
    int success;
    if (setjmp(buf) == 0) {
        value = asnix_force_for_print(args[0]);
        success = 1;
    } else {
        asnix_tryeval_unwind_inprogress(saved_inprogress_head);
        value = asnix_make_imm(ASNIX_IMM_FALSE);
        success = 0;
    }
    asnix_tryeval_pop();
    asnix_valstack_limit = saved_valstack_limit;
    asnix_retsp_limit = saved_retsp_limit;
    asnix_force_depth = saved_force_depth;

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

