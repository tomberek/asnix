#include "aterm.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
}

static void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    b->buf[b->len++] = c;
}

static void buf_puts(Buf *b, const char *s) {
    size_t n = strlen(s);
    buf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

/* Nix's ATerm escaping: only \n \r \t \" \\ are ever escaped; every other
 * byte (including non-ASCII/non-UTF-8) is written verbatim. Includes the
 * surrounding quotes. */
static void buf_put_escaped(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\n': buf_puts(b, "\\n"); break;
            case '\r': buf_puts(b, "\\r"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '"': buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            default: buf_putc(b, (char)*p); break;
        }
    }
    buf_putc(b, '"');
}

char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs, const char *system,
                                    const char *builder, const char **args, size_t nargs, const AtermEnvVar *env,
                                    size_t nenv) {
    Buf b = { 0 };

    buf_puts(&b, "Derive([");
    for (size_t i = 0; i < noutputs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, outputs[i].name);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].path);
        buf_puts(&b, ",\"\",\"\")");
    }
    buf_puts(&b, "],[],[],");
    buf_put_escaped(&b, system);
    buf_putc(&b, ',');
    buf_put_escaped(&b, builder);
    buf_putc(&b, ',');

    buf_putc(&b, '[');
    for (size_t i = 0; i < nargs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, args[i]);
    }
    buf_puts(&b, "],[");
    for (size_t i = 0; i < nenv; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, env[i].key);
        buf_putc(&b, ',');
        buf_put_escaped(&b, env[i].value);
        buf_putc(&b, ')');
    }
    buf_puts(&b, "])");

    buf_putc(&b, '\0'); /* buf_reserve always keeps +1 slack for this */
    return b.buf;
}
