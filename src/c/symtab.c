#include "symtab.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    size_t len;
} SymEntry;

static SymEntry *g_symbols = NULL;
static uint32_t g_count = 0, g_cap = 0;

static uint32_t *g_table = NULL;
static size_t g_table_cap = 0;
#define EMPTY_SLOT 0xFFFFFFFFu

static uint64_t hash_bytes(const char *s, size_t len) {
    uint64_t h = 0;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        h ^= w;
        h *= 0x517cc1b727220a95ULL;
    }
    if (i + 4 <= len) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        h ^= (uint64_t)w;
        h *= 0x517cc1b727220a95ULL;
        i += 4;
    }
    for (; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static void table_rehash(size_t new_cap) {
    uint32_t *new_table = malloc(new_cap * sizeof(uint32_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = EMPTY_SLOT;
    for (uint32_t id = 0; id < g_count; id++) {
        uint64_t h = hash_bytes(g_symbols[id].name, g_symbols[id].len);
        size_t slot = h & (new_cap - 1);
        while (new_table[slot] != EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = id;
    }
    free(g_table);
    g_table = new_table;
    g_table_cap = new_cap;
}

uint32_t asnix_intern(const char *name, size_t len) {
    if (g_table_cap == 0) table_rehash(16);

    uint64_t h = hash_bytes(name, len);
    size_t slot = h & (g_table_cap - 1);
    while (g_table[slot] != EMPTY_SLOT) {
        uint32_t id = g_table[slot];
        if (g_symbols[id].len == len && memcmp(g_symbols[id].name, name, len) == 0) return id;
        slot = (slot + 1) & (g_table_cap - 1);
    }

    if (g_count == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 16;
        g_symbols = realloc(g_symbols, g_cap * sizeof(SymEntry));
    }
    uint32_t id = g_count++;
    char *owned = malloc(len);
    memcpy(owned, name, len);
    g_symbols[id] = (SymEntry){ .name = owned, .len = len };
    g_table[slot] = id;

    if (g_count * 4 > g_table_cap * 3) table_rehash(g_table_cap * 2);
    return id;
}

const char *asnix_sym_name(uint32_t id, size_t *out_len) {
    *out_len = g_symbols[id].len;
    return g_symbols[id].name;
}

