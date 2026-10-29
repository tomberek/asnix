#include "builtins_toml.h"

#include <stdint.h>

#include "builtins_shared.h"
#include "toml.h"
#include "value_util.h"

static Value prim_fromTOML(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "fromTOML");
    return asnix_toml_parse(bytes, (size_t)len);
}

static Value build_fromTOML(void) { return asnix_bshared_make_primop((void *)prim_fromTOML, 1); }

Value asnix_make_fromTOML_primop(void) { return asnix_bshared_make_primop((void *)prim_fromTOML, 1); }

const AsnixBuiltinMiscDef ASNIX_TOML_BUILTINS[] = {
    { "fromTOML", build_fromTOML },
};

const size_t ASNIX_TOML_BUILTINS_COUNT = sizeof(ASNIX_TOML_BUILTINS) / sizeof(ASNIX_TOML_BUILTINS[0]);

