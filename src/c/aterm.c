#include "aterm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
} Buf;

static Buf g_aterm_buf = { 0 };

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

static void buf_put_bytes(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

#define buf_put_lit(b, lit) buf_put_bytes((b), (lit), sizeof(lit) - 1)

static size_t find_escape_aterm(const unsigned char *s, size_t len) {
    static const uint64_t LO = 0x0101010101010101ULL;
    static const uint64_t HI = 0x8080808080808080ULL;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        uint64_t q = x ^ (LO * (uint64_t)'"');
        uint64_t bs = x ^ (LO * (uint64_t)'\\');
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

typedef struct {
    const char *key;
    char *val;
    size_t len;
} AtermCacheSlot;

struct AtermEscapeCache {
    AtermCacheSlot *slots;
    size_t cap;
    size_t count;
};

static size_t aterm_ptr_hash(const char *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

AtermEscapeCache *asnix_aterm_escape_cache_new(size_t hint) {
    size_t cap = 64;
    while (cap < hint * 2) cap *= 2;
    AtermEscapeCache *c = malloc(sizeof(*c));
    c->slots = calloc(cap, sizeof(AtermCacheSlot));
    c->cap = cap;
    c->count = 0;
    return c;
}

void asnix_aterm_escape_cache_free(AtermEscapeCache *c) {
    if (!c) return;
    for (size_t i = 0; i < c->cap; i++) free(c->slots[i].val);
    free(c->slots);
    free(c);
}

static int aterm_cache_lookup(AtermEscapeCache *c, const char *s, char **out_val, size_t *out_len) {
    size_t mask = c->cap - 1;
    size_t i = aterm_ptr_hash(s) & mask;
    for (;;) {
        if (c->slots[i].key == NULL) return 0;
        if (c->slots[i].key == s) {
            *out_val = c->slots[i].val;
            *out_len = c->slots[i].len;
            return 1;
        }
        i = (i + 1) & mask;
    }
}

static void aterm_cache_grow(AtermEscapeCache *c) {
    size_t old_cap = c->cap;
    AtermCacheSlot *old_slots = c->slots;
    size_t new_cap = old_cap * 2;
    AtermCacheSlot *new_slots = calloc(new_cap, sizeof(AtermCacheSlot));
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old_slots[i].key) continue;
        size_t j = aterm_ptr_hash(old_slots[i].key) & mask;
        while (new_slots[j].key) j = (j + 1) & mask;
        new_slots[j] = old_slots[i];
    }
    free(old_slots);
    c->slots = new_slots;
    c->cap = new_cap;
}

static void aterm_cache_insert(AtermEscapeCache *c, const char *s, char *val, size_t len) {
    if ((c->count + 1) * 10 > c->cap * 7) aterm_cache_grow(c);
    size_t mask = c->cap - 1;
    size_t i = aterm_ptr_hash(s) & mask;
    while (c->slots[i].key) i = (i + 1) & mask;
    c->slots[i].key = s;
    c->slots[i].val = val;
    c->slots[i].len = len;
    c->count++;
}

static void buf_put_escaped_raw(Buf *b, const char *s) {
    buf_putc(b, '"');
    size_t len = strlen(s);
    const unsigned char *p = (const unsigned char *)s;
    size_t pos = 0;
    for (;;) {
        size_t esc = pos + find_escape_aterm(p + pos, len - pos);
        if (esc != pos) buf_put_bytes(b, (const char *)(p + pos), esc - pos);
        if (esc == len) break;
        switch (p[esc]) {
            case '\n': buf_put_lit(b, "\\n"); break;
            case '\r': buf_put_lit(b, "\\r"); break;
            case '\t': buf_put_lit(b, "\\t"); break;
            case '"': buf_put_lit(b, "\\\""); break;
            case '\\': buf_put_lit(b, "\\\\"); break;
        }
        pos = esc + 1;
    }
    buf_putc(b, '"');
}

static void buf_put_escaped(Buf *b, const char *s, AtermEscapeCache *cache) {
    if (cache) {
        char *cval;
        size_t clen;
        if (aterm_cache_lookup(cache, s, &cval, &clen)) {
            buf_put_bytes(b, cval, clen);
            return;
        }
    }
    size_t start = b->len;
    buf_put_escaped_raw(b, s);
    if (cache) {
        size_t clen = b->len - start;
        char *cval = malloc(clen ? clen : 1);
        memcpy(cval, b->buf + start, clen);
        aterm_cache_insert(cache, s, cval, clen);
    }
}

char *asnix_build_derivation_aterm_cached(const AtermOutput *outputs, size_t noutputs,
                                           const AtermInputDrv *input_drvs, size_t ninput_drvs,
                                           const char **input_srcs, size_t ninput_srcs, const char *system,
                                           const char *builder, const char **args, size_t nargs,
                                           const AtermEnvVar *env, size_t nenv, AtermEscapeCache *cache) {
    Buf *bp = &g_aterm_buf;
    bp->len = 0;
    Buf b = *bp;

    buf_put_lit(&b, "Derive([");
    for (size_t i = 0; i < noutputs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, outputs[i].name, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].path, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash_algo ? outputs[i].hash_algo : "", cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash ? outputs[i].hash : "", cache);
        buf_putc(&b, ')');
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < ninput_drvs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, input_drvs[i].drv_path, cache);
        buf_put_lit(&b, ",[");
        for (size_t j = 0; j < input_drvs[i].noutput_names; j++) {
            if (j > 0) buf_putc(&b, ',');
            buf_put_escaped(&b, input_drvs[i].output_names[j], cache);
        }
        buf_put_lit(&b, "])");
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < ninput_srcs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, input_srcs[i], cache);
    }
    buf_put_lit(&b, "],");
    buf_put_escaped(&b, system, cache);
    buf_putc(&b, ',');
    buf_put_escaped(&b, builder, cache);
    buf_putc(&b, ',');

    buf_putc(&b, '[');
    for (size_t i = 0; i < nargs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, args[i], cache);
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < nenv; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, env[i].key, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, env[i].value, cache);
        buf_putc(&b, ')');
    }
    buf_put_lit(&b, "])");

    buf_putc(&b, '\0');

    *bp = b;

    char *out = malloc(b.len);
    memcpy(out, b.buf, b.len);
    return out;
}

char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs,
                                    const AtermInputDrv *input_drvs, size_t ninput_drvs, const char **input_srcs,
                                    size_t ninput_srcs, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv) {
    return asnix_build_derivation_aterm_cached(outputs, noutputs, input_drvs, ninput_drvs, input_srcs, ninput_srcs,
                                                system, builder, args, nargs, env, nenv, NULL);
}

