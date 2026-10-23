#include "aterm.h"

#include <stdint.h>
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

static void buf_put_bytes(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

/* perf (M138): SWAR scan for the aterm escape set (\n \r \t \" \\),
 * processing 8 bytes per iteration instead of one branchy compare-chain
 * per byte. Ported from iets's own `find_escape` (~/gnix/iets/src/store/
 * drv.rs) -- confirmed via its own exhaustive-fuzz unit test
 * (`find_escape_finds_the_first_byte_to_escape`) and re-derived here
 * against the two underlying bit-twiddling identities independently
 * (Sean Eron Anderson's "bit twiddling hacks": "determine if a word has
 * a byte less than n" -- correct for ANY byte value 0-255, not just
 * ASCII, as long as n<=128; and the companion "has a zero byte" trick
 * used here to detect an exact byte match via XOR-then-haszero). Pure
 * filter: a chunk flagged "maybe" is re-scanned byte-by-byte to find the
 * real position (handles the trick's rare false positives); a chunk
 * flagged "no" is trusted outright (the identities have no false
 * negatives), so output is byte-for-byte identical to a plain scalar
 * scan for every input, including non-ASCII/non-UTF-8 bytes. */
static size_t find_escape_aterm(const unsigned char *s, size_t len) {
    static const uint64_t LO = 0x0101010101010101ULL;
    static const uint64_t HI = 0x8080808080808080ULL;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        uint64_t q = x ^ (LO * (uint64_t)'"');
        uint64_t bs = x ^ (LO * (uint64_t)'\\');
        /* any byte < 0x0e (covers \t=0x09,\n=0x0a,\r=0x0d) OR any byte
         * == '"' OR any byte == '\\'. */
        uint64_t any = ((x - LO * 0x0e) & ~x) | ((q - LO) & ~q) | ((bs - LO) & ~bs);
        if (any & HI) {
            for (size_t j = i; j < i + 8; j++) {
                unsigned char c = s[j];
                if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\\') return j;
            }
        }
    }
    for (; i < len; i++) {
        unsigned char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\\') return i;
    }
    return len;
}

/* Nix's ATerm escaping: only \n \r \t \" \\ are ever escaped; every other
 * byte (including non-ASCII/non-UTF-8) is written verbatim. Includes the
 * surrounding quotes. Output is byte-for-byte identical to a plain
 * per-byte scan-and-switch loop. */
static void buf_put_escaped(Buf *b, const char *s) {
    buf_putc(b, '"');
    size_t len = strlen(s);
    const unsigned char *p = (const unsigned char *)s;
    size_t pos = 0;
    for (;;) {
        size_t esc = pos + find_escape_aterm(p + pos, len - pos);
        if (esc != pos) buf_put_bytes(b, (const char *)(p + pos), esc - pos);
        if (esc == len) break;
        switch (p[esc]) {
            case '\n': buf_puts(b, "\\n"); break;
            case '\r': buf_puts(b, "\\r"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '"': buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
        }
        pos = esc + 1;
    }
    buf_putc(b, '"');
}

char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs,
                                    const AtermInputDrv *input_drvs, size_t ninput_drvs, const char **input_srcs,
                                    size_t ninput_srcs, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv) {
    Buf b = { 0 };

    buf_puts(&b, "Derive([");
    for (size_t i = 0; i < noutputs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, outputs[i].name);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].path);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash_algo ? outputs[i].hash_algo : "");
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash ? outputs[i].hash : "");
        buf_putc(&b, ')');
    }
    buf_puts(&b, "],[");
    for (size_t i = 0; i < ninput_drvs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, input_drvs[i].drv_path);
        buf_puts(&b, ",[");
        for (size_t j = 0; j < input_drvs[i].noutput_names; j++) {
            if (j > 0) buf_putc(&b, ',');
            buf_put_escaped(&b, input_drvs[i].output_names[j]);
        }
        buf_puts(&b, "])");
    }
    buf_puts(&b, "],[");
    for (size_t i = 0; i < ninput_srcs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, input_srcs[i]);
    }
    buf_puts(&b, "],");
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
