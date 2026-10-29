#include "builtins_closure.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

typedef struct {
    Value *data;
    size_t len, cap;
} ValueVec;

static void vv_push(ValueVec *v, Value x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = realloc(v->data, v->cap * sizeof(Value));
    }
    v->data[v->len++] = x;
}

typedef enum { KC_INT, KC_STR, KC_LIST, KC_FLOAT, KC_OTHER } KeyClass;

static KeyClass key_class(Value v) {
    int t = asnix_tag_of(v);
    if (t == ASNIX_TAG_INT) return KC_INT;
    if (t == ASNIX_TAG_PTR && asnix_value_is_float(v)) return KC_FLOAT;
    if (t == ASNIX_TAG_PTR && !asnix_value_is_path(v)) return KC_STR;
    if (t == ASNIX_TAG_LIST) return KC_LIST;
    return KC_OTHER;
}

static int classes_comparable(KeyClass a, KeyClass b) {
    return (a == KC_INT && b == KC_INT) || (a == KC_STR && b == KC_STR) || (a == KC_LIST && b == KC_LIST) ||
           (a == KC_FLOAT && b == KC_FLOAT);
}

static uint64_t closure_key_hash(Value v) {
    uint64_t h = 1469598103934665603ULL;
    int t = asnix_tag_of(v);
    if (t == ASNIX_TAG_INT) {
        h ^= (uint64_t)asnix_int_value(v);
        h *= 0x100000001b3ULL;
        return h;
    }
    if (asnix_value_is_float(v)) {
        double d = asnix_float_value(v);
        if (d == 0.0) d = 0.0;
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        h ^= bits;
        h *= 0x100000001b3ULL;
        return h;
    }
    if (t == ASNIX_TAG_LIST) {
        int64_t n = asnix_bshared_list_count(v);
        const Value *elems = asnix_bshared_list_elems(v);
        h ^= (uint64_t)n;
        h *= 0x100000001b3ULL;
        for (int64_t i = 0; i < n; i++) {
            uint64_t eh = closure_key_hash(asnix_force_for_print(elems[i]));
            h ^= eh;
            h *= 0x100000001b3ULL;
        }
        return h;
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(v, &len);
    for (int64_t i = 0; i < len; i++) {
        h ^= (uint8_t)bytes[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

typedef struct {
    uint64_t hash;
    Value key;
    int used;
} ClosureSeenSlot;

typedef struct {
    ClosureSeenSlot *slots;
    size_t cap;
    size_t count;
} ClosureSeenSet;

static void seen_set_init(ClosureSeenSet *s) {
    s->cap = 64;
    s->slots = calloc(s->cap, sizeof(ClosureSeenSlot));
    s->count = 0;
}

static void seen_set_insert_raw(ClosureSeenSet *s, uint64_t hash, Value key) {
    size_t mask = s->cap - 1;
    size_t i = (size_t)hash & mask;
    while (s->slots[i].used) i = (i + 1) & mask;
    s->slots[i].used = 1;
    s->slots[i].hash = hash;
    s->slots[i].key = key;
    s->count++;
}

static void seen_set_grow(ClosureSeenSet *s) {
    size_t old_cap = s->cap;
    ClosureSeenSlot *old = s->slots;
    s->cap = old_cap * 2;
    s->slots = calloc(s->cap, sizeof(ClosureSeenSlot));
    s->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].used) seen_set_insert_raw(s, old[i].hash, old[i].key);
    }
    free(old);
}

static int seen_set_probe_or_insert(ClosureSeenSet *s, Value key) {
    if (s->count * 4 >= s->cap * 3) seen_set_grow(s);
    uint64_t h = closure_key_hash(key);
    size_t mask = s->cap - 1;
    size_t i = (size_t)h & mask;
    while (s->slots[i].used) {
        if (s->slots[i].hash == h && asnix_bshared_value_eq(s->slots[i].key, key)) return 1;
        i = (i + 1) & mask;
    }
    s->slots[i].used = 1;
    s->slots[i].hash = h;
    s->slots[i].key = key;
    s->count++;
    return 0;
}

static Value prim_genericClosure(const Value *args, int64_t argc) {
    (void)argc;
    Value input = args[0];
    ASNIX_REQUIRE_ATTRS(input, "genericClosure");

    uint32_t sym_startSet = asnix_intern("startSet", 8);
    uint32_t sym_operator = asnix_intern("operator", 8);
    uint32_t sym_key = asnix_intern("key", 3);

    int64_t ss_idx = asnix_bshared_attrs_find(input, sym_startSet);
    if (ss_idx < 0) asnix_bshared_fatal("genericClosure", "attribute 'startSet' is missing");
    Value start_set = asnix_force_for_print(asnix_bshared_attrs_value(input, ss_idx));
    ASNIX_REQUIRE_LIST(start_set, "genericClosure");

    int64_t op_idx = asnix_bshared_attrs_find(input, sym_operator);
    if (op_idx < 0) asnix_bshared_fatal("genericClosure", "attribute 'operator' is missing");
    Value operator_fn = asnix_force_for_print(asnix_bshared_attrs_value(input, op_idx));

    ValueVec queue = {0}, result = {0};
    ClosureSeenSet seen;
    seen_set_init(&seen);
    int have_class = 0;
    KeyClass class0 = KC_OTHER;
    int64_t n0 = asnix_bshared_list_count(start_set);
    const Value *elems0 = asnix_bshared_list_elems(start_set);
    for (int64_t i = 0; i < n0; i++) vv_push(&queue, elems0[i]);

    size_t head = 0;
    while (head < queue.len) {
        Value item = asnix_force_for_print(queue.data[head++]);
        if (asnix_tag_of(item) != ASNIX_TAG_ATTRS) {
            asnix_bshared_fatal("genericClosure", "a work-set element is not an attrset");
        }
        int64_t k_idx = asnix_bshared_attrs_find(item, sym_key);
        if (k_idx < 0) {
            asnix_bshared_fatal("genericClosure", "a work-set element is missing the 'key' attribute");
        }
        Value key = asnix_force_for_print(asnix_bshared_attrs_value(item, k_idx));

        KeyClass kc = key_class(key);
        if (!have_class) {
            have_class = 1;
            class0 = kc;
        } else if (!classes_comparable(class0, kc)) {
            asnix_bshared_fatal("genericClosure", "cannot compare two 'key' values (incompatible or uncomparable types)");
        }
        int already_seen = (kc == KC_OTHER) ? 0 : seen_set_probe_or_insert(&seen, key);
        if (already_seen) continue;
        vv_push(&result, item);

        Value next = asnix_apply(operator_fn, &item, 1);
        if (asnix_tag_of(next) != ASNIX_TAG_LIST) {
            asnix_bshared_fatal("genericClosure", "'operator' did not return a list");
        }
        int64_t ncount = asnix_bshared_list_count(next);
        const Value *nelems = asnix_bshared_list_elems(next);
        for (int64_t i = 0; i < ncount; i++) vv_push(&queue, nelems[i]);
    }

    Value out = asnix_bshared_make_list(result.data, (int64_t)result.len);
    free(queue.data);
    free(seen.slots);
    free(result.data);
    return out;
}

static Value build_genericClosure(void) { return asnix_bshared_make_primop((void *)prim_genericClosure, 1); }

const AsnixBuiltinMiscDef ASNIX_CLOSURE_BUILTINS[] = {
    { "genericClosure", build_genericClosure },
};

const size_t ASNIX_CLOSURE_BUILTINS_COUNT = sizeof(ASNIX_CLOSURE_BUILTINS) / sizeof(ASNIX_CLOSURE_BUILTINS[0]);

