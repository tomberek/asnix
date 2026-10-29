#include "regex_engine.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    RN_LIT,
    RN_ANY,
    RN_CLASS,
    RN_BOL,
    RN_EOL,
    RN_SEQ,
    RN_ALT,
    RN_GROUP,
    RN_REP
} RNKind;

typedef struct RNode RNode;
struct RNode {
    RNKind kind;
    char lit;
    unsigned char *cls;
    RNode **kids;
    int nkids;
    RNode *child;
    int group_idx;
    int rep_min, rep_max;
};

struct AsnixCustomRegex {
    RNode *root;
    int64_t nsub;
};

static RNode *new_node(RNKind k) {
    RNode *n = calloc(1, sizeof *n);
    n->kind = k;
    return n;
}

static void free_node(RNode *n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) free_node(n->kids[i]);
    free(n->kids);
    free_node(n->child);
    free(n->cls);
    free(n);
}

typedef struct {
    const char *p;
    int64_t pos, len;
    int next_group;
    int error;
} Parser;

static int pk(const Parser *ps) { return ps->pos < ps->len ? (unsigned char)ps->p[ps->pos] : -1; }
static int pk2(const Parser *ps) { return ps->pos + 1 < ps->len ? (unsigned char)ps->p[ps->pos + 1] : -1; }

static int collate_lookup(const char *name, int64_t namelen, unsigned char *out) {
    if (namelen == 1) {
        if (!isalpha((unsigned char)name[0])) return 0;
        *out = (unsigned char)name[0];
        return 1;
    }
    static const struct { const char *name; unsigned char ch; } table[] = {
        { "NUL", 0 }, { "alert", '\a' }, { "backspace", '\b' }, { "tab", '\t' },
        { "newline", '\n' }, { "vertical-tab", '\v' }, { "form-feed", '\f' },
        { "carriage-return", '\r' }, { "space", ' ' },
        { "exclamation-mark", '!' }, { "quotation-mark", '"' }, { "number-sign", '#' },
        { "dollar-sign", '$' }, { "percent-sign", '%' }, { "ampersand", '&' },
        { "apostrophe", '\'' }, { "left-parenthesis", '(' }, { "right-parenthesis", ')' },
        { "asterisk", '*' }, { "plus-sign", '+' }, { "comma", ',' },
        { "hyphen", '-' }, { "hyphen-minus", '-' }, { "period", '.' }, { "full-stop", '.' },
        { "slash", '/' }, { "solidus", '/' }, { "colon", ':' }, { "semicolon", ';' },
        { "less-than-sign", '<' }, { "equals-sign", '=' }, { "greater-than-sign", '>' },
        { "question-mark", '?' }, { "commercial-at", '@' },
        { "left-square-bracket", '[' }, { "backslash", '\\' }, { "reverse-solidus", '\\' },
        { "right-square-bracket", ']' }, { "circumflex", '^' }, { "circumflex-accent", '^' },
        { "underscore", '_' }, { "low-line", '_' }, { "grave-accent", '`' },
        { "left-brace", '{' }, { "left-curly-bracket", '{' }, { "vertical-line", '|' },
        { "right-brace", '}' }, { "right-curly-bracket", '}' }, { "tilde", '~' },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if ((int64_t)strlen(table[i].name) == namelen && memcmp(table[i].name, name, (size_t)namelen) == 0) {
            *out = table[i].ch;
            return 1;
        }
    }
    return 0;
}

static int class_test(const char *name, int64_t namelen, unsigned char ch, int *recognized) {
    char lname[16];
    *recognized = 0;
    if (namelen <= 0 || namelen >= (int64_t)sizeof lname) return 0;
    for (int64_t i = 0; i < namelen; i++) lname[i] = (char)tolower((unsigned char)name[i]);
    lname[namelen] = '\0';
    *recognized = 1;
    if (!strcmp(lname, "alpha")) return isalpha(ch) != 0;
    if (!strcmp(lname, "digit")) return isdigit(ch) != 0;
    if (!strcmp(lname, "alnum")) return isalnum(ch) != 0;
    if (!strcmp(lname, "upper")) return isupper(ch) != 0;
    if (!strcmp(lname, "lower")) return islower(ch) != 0;
    if (!strcmp(lname, "space")) return isspace(ch) != 0;
    if (!strcmp(lname, "blank")) return ch == ' ' || ch == '\t';
    if (!strcmp(lname, "cntrl")) return iscntrl(ch) != 0;
    if (!strcmp(lname, "punct")) return ispunct(ch) != 0;
    if (!strcmp(lname, "print")) return isprint(ch) != 0;
    if (!strcmp(lname, "graph")) return isgraph(ch) != 0;
    if (!strcmp(lname, "xdigit")) return isxdigit(ch) != 0;
    if (!strcmp(lname, "w")) return isalnum(ch) != 0 || ch == '_';
    *recognized = 0;
    return 0;
}

static int parse_range_endpoint(Parser *ps, unsigned char *out) {
    if (pk(ps) == '[' && pk2(ps) == '.') return 0;
    if (pk(ps) < 0) return 0;
    *out = (unsigned char)pk(ps);
    ps->pos++;
    return 1;
}

static RNode *parse_bracket(Parser *ps) {
    ps->pos++;
    int negate = 0;
    if (pk(ps) == '^') {
        negate = 1;
        ps->pos++;
    }
    unsigned char *cls = calloc(256, 1);
    int first = 1;
    for (;;) {
        int c = pk(ps);
        if (c < 0) { ps->error = 1; free(cls); return NULL; }
        if (c == ']' && !first) break;
        first = 0;
        if (c == '[' && pk2(ps) == '.') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == '.' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            unsigned char ch;
            if (!collate_lookup(ps->p + namestart, namelen, &ch)) { ps->error = 1; free(cls); return NULL; }
            ps->pos = j + 2;
            if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) {
                ps->pos++;
                unsigned char hi;
                if (!parse_range_endpoint(ps, &hi)) { ps->error = 1; free(cls); return NULL; }
                if (hi < ch) { ps->error = 1; free(cls); return NULL; }
                for (int v = ch; v <= hi; v++) cls[v] = 1;
            } else {
                cls[ch] = 1;
            }
            continue;
        }
        if (c == '[' && pk2(ps) == ':') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == ':' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            int recognized;
            class_test(ps->p + namestart, namelen, 'a', &recognized);
            if (!recognized) { ps->error = 1; free(cls); return NULL; }
            for (int v = 0; v < 256; v++) {
                int rec;
                if (class_test(ps->p + namestart, namelen, (unsigned char)v, &rec)) cls[v] = 1;
            }
            ps->pos = j + 2;
            continue;
        }
        if (c == '[' && pk2(ps) == '=') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == '=' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            unsigned char ch;
            if (!collate_lookup(ps->p + namestart, namelen, &ch)) { ps->error = 1; free(cls); return NULL; }
            cls[ch] = 1;
            if (isalpha(ch)) cls[(unsigned char)(islower(ch) ? toupper(ch) : tolower(ch))] = 1;
            ps->pos = j + 2;
            if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) { ps->error = 1; free(cls); return NULL; }
            continue;
        }
        unsigned char ch = (unsigned char)c;
        ps->pos++;
        if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) {
            ps->pos++;
            unsigned char hi;
            if (!parse_range_endpoint(ps, &hi)) { ps->error = 1; free(cls); return NULL; }
            if (hi < ch) { ps->error = 1; free(cls); return NULL; }
            for (int v = ch; v <= hi; v++) cls[v] = 1;
        } else {
            cls[ch] = 1;
        }
    }
    ps->pos++;
    if (negate) for (int v = 0; v < 256; v++) cls[v] = !cls[v];
    RNode *n = new_node(RN_CLASS);
    n->cls = cls;
    return n;
}

static int parse_digits(Parser *ps, int *out) {
    int64_t start = ps->pos;
    int v = 0, any = 0;
    while (pk(ps) >= 0 && isdigit(pk(ps))) {
        v = v * 10 + (pk(ps) - '0');
        if (v > 100000) v = 100000;
        ps->pos++;
        any = 1;
    }
    if (!any) { ps->pos = start; return 0; }
    *out = v;
    return 1;
}

static int parse_interval(Parser *ps, int *min_out, int *max_out) {
    ps->pos++;
    int lo;
    if (!parse_digits(ps, &lo)) return 0;
    int hi;
    if (pk(ps) == ',') {
        ps->pos++;
        if (pk(ps) == '}') {
            hi = -1;
        } else if (!parse_digits(ps, &hi)) {
            return 0;
        }
    } else {
        hi = lo;
    }
    if (pk(ps) != '}') return 0;
    ps->pos++;
    *min_out = lo;
    *max_out = hi;
    return 1;
}

static RNode *parse_alt(Parser *ps);

static RNode *parse_atom(Parser *ps) {
    int c = pk(ps);
    if (c < 0) { ps->error = 1; return NULL; }
    if (c == '(') {
        ps->pos++;
        int gidx = ps->next_group++;
        RNode *inner = parse_alt(ps);
        if (!inner) return NULL;
        if (pk(ps) != ')') { ps->error = 1; free_node(inner); return NULL; }
        ps->pos++;
        RNode *g = new_node(RN_GROUP);
        g->child = inner;
        g->group_idx = gidx;
        return g;
    }
    if (c == '.') { ps->pos++; return new_node(RN_ANY); }
    if (c == '^') { ps->pos++; return new_node(RN_BOL); }
    if (c == '$') { ps->pos++; return new_node(RN_EOL); }
    if (c == '[') return parse_bracket(ps);
    if (c == '\\') {
        ps->pos++;
        int e = pk(ps);
        if (e < 0) { ps->error = 1; return NULL; }
        ps->pos++;
        RNode *n = new_node(RN_LIT);
        n->lit = (char)e;
        return n;
    }
    ps->pos++;
    RNode *n = new_node(RN_LIT);
    n->lit = (char)c;
    return n;
}

static RNode *parse_piece(Parser *ps) {
    RNode *atom = parse_atom(ps);
    if (!atom) return NULL;
    for (;;) {
        int c = pk(ps);
        int lo, hi;
        if (c == '*') { lo = 0; hi = -1; ps->pos++; }
        else if (c == '+') { lo = 1; hi = -1; ps->pos++; }
        else if (c == '?') { lo = 0; hi = 1; ps->pos++; }
        else if (c == '{') {
            int64_t save = ps->pos;
            if (!parse_interval(ps, &lo, &hi)) { ps->pos = save; break; }
        } else {
            break;
        }
        RNode *rep = new_node(RN_REP);
        rep->child = atom;
        rep->rep_min = lo;
        rep->rep_max = hi;
        atom = rep;
    }
    return atom;
}

static RNode *parse_seq(Parser *ps) {
    RNode **kids = NULL;
    int n = 0, cap = 0;
    for (;;) {
        int c = pk(ps);
        if (c < 0 || c == '|' || c == ')') break;
        RNode *piece = parse_piece(ps);
        if (!piece) {
            for (int i = 0; i < n; i++) free_node(kids[i]);
            free(kids);
            return NULL;
        }
        if (n >= cap) {
            cap = cap ? cap * 2 : 4;
            kids = realloc(kids, sizeof(RNode *) * (size_t)cap);
        }
        kids[n++] = piece;
    }
    RNode *seq = new_node(RN_SEQ);
    seq->kids = kids;
    seq->nkids = n;
    return seq;
}

static RNode *parse_alt(Parser *ps) {
    RNode *first = parse_seq(ps);
    if (!first) return NULL;
    if (pk(ps) != '|') return first;
    RNode **kids = malloc(sizeof(RNode *) * 2);
    int n = 1;
    kids[0] = first;
    while (pk(ps) == '|') {
        ps->pos++;
        RNode *br = parse_seq(ps);
        if (!br) {
            for (int i = 0; i < n; i++) free_node(kids[i]);
            free(kids);
            return NULL;
        }
        kids = realloc(kids, sizeof(RNode *) * (size_t)(n + 1));
        kids[n++] = br;
    }
    RNode *alt = new_node(RN_ALT);
    alt->kids = kids;
    alt->nkids = n;
    return alt;
}

AsnixCustomRegex *asnix_regex_compile(const char *pattern, int64_t len, int64_t *nsub_out) {
    Parser ps = { pattern, 0, len, 1, 0 };
    RNode *root = parse_alt(&ps);
    if (!root || ps.error || ps.pos != ps.len) {
        free_node(root);
        return NULL;
    }
    AsnixCustomRegex *re = malloc(sizeof *re);
    re->root = root;
    re->nsub = ps.next_group - 1;
    if (nsub_out) *nsub_out = re->nsub;
    return re;
}

void asnix_regex_free(AsnixCustomRegex *re) {
    if (!re) return;
    free_node(re->root);
    free(re);
}

typedef struct Cont Cont;
typedef enum { K_SEQREST, K_CLOSEGROUP, K_STARITER } KKind;
struct Cont {
    KKind kind;
    union {
        struct { RNode * const *kids; int n; int idx; } seqrest;
        struct { int idx; int64_t start; } closegroup;
        struct { const RNode *rep; int reps_done; int64_t start_pos; } stariter;
    } u;
    const Cont *next;
};

typedef struct {
    const char *subject;
    int64_t slen;
    int notbol;
    AsnixRegexGroup *groups;
    int64_t nsub;
    int anchored;
    int64_t required_end;
    int64_t best_end;
    int found;
    int64_t *budget;
} MCtx;

static int m(const RNode *n, int64_t pos, const Cont *k, MCtx *ctx);
static int match_rep(const RNode *rep, int64_t pos, const Cont *k, MCtx *ctx, int reps_done, int blocked);

static int run_cont(const Cont *k, int64_t pos, MCtx *ctx) {
    if (!k) {
        if (ctx->anchored) return pos == ctx->required_end;
        ctx->found = 1;
        ctx->best_end = pos;
        return 1;
    }
    switch (k->kind) {
        case K_SEQREST:
            if (k->u.seqrest.idx >= k->u.seqrest.n) return run_cont(k->next, pos, ctx);
            {
                Cont nk;
                nk.kind = K_SEQREST;
                nk.u.seqrest.kids = k->u.seqrest.kids;
                nk.u.seqrest.n = k->u.seqrest.n;
                nk.u.seqrest.idx = k->u.seqrest.idx + 1;
                nk.next = k->next;
                return m(k->u.seqrest.kids[k->u.seqrest.idx], pos, &nk, ctx);
            }
        case K_CLOSEGROUP: {
            if (!ctx->groups) return run_cont(k->next, pos, ctx);
            int idx = k->u.closegroup.idx;
            AsnixRegexGroup save = ctx->groups[idx - 1];
            ctx->groups[idx - 1].so = k->u.closegroup.start;
            ctx->groups[idx - 1].eo = pos;
            int r = run_cont(k->next, pos, ctx);
            if (!r) ctx->groups[idx - 1] = save;
            return r;
        }
        case K_STARITER: {
            int64_t start_pos = k->u.stariter.start_pos;
            int blocked = (pos == start_pos);
            return match_rep(k->u.stariter.rep, pos, k->next, ctx, k->u.stariter.reps_done + 1, blocked);
        }
    }
    return 0;
}

static int m(const RNode *n, int64_t pos, const Cont *k, MCtx *ctx) {
    if (ctx->budget) (*ctx->budget)--;
    switch (n->kind) {
        case RN_LIT:
            if (pos < ctx->slen && ctx->subject[pos] == n->lit) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_ANY:
            if (pos < ctx->slen) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_CLASS:
            if (pos < ctx->slen && n->cls[(unsigned char)ctx->subject[pos]]) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_BOL:
            if (!ctx->notbol && pos == 0) return run_cont(k, pos, ctx);
            return 0;
        case RN_EOL:
            if (pos == ctx->slen) return run_cont(k, pos, ctx);
            return 0;
        case RN_SEQ:
            if (n->nkids == 0) return run_cont(k, pos, ctx);
            {
                Cont c;
                c.kind = K_SEQREST;
                c.u.seqrest.kids = n->kids;
                c.u.seqrest.n = n->nkids;
                c.u.seqrest.idx = 1;
                c.next = k;
                return m(n->kids[0], pos, &c, ctx);
            }
        case RN_ALT:
            if (ctx->anchored || n->nkids <= 1 || (ctx->budget && *ctx->budget <= 0)) {
                for (int i = 0; i < n->nkids; i++)
                    if (m(n->kids[i], pos, k, ctx)) return 1;
                return 0;
            }
            {
                AsnixRegexGroup *saved = NULL;
                if (ctx->groups && ctx->nsub > 0) {
                    saved = malloc((size_t)ctx->nsub * sizeof *saved);
                    memcpy(saved, ctx->groups, (size_t)ctx->nsub * sizeof *saved);
                }
                int64_t best_end = -1;
                int best_i = -1;
                for (int i = 0; i < n->nkids; i++) {
                    if (saved) memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved);
                    MCtx trial = *ctx;
                    trial.found = 0;
                    trial.best_end = -1;
                    if (m(n->kids[i], pos, k, &trial) && trial.best_end > best_end) {
                        best_end = trial.best_end;
                        best_i = i;
                    }
                }
                if (best_i < 0) {
                    if (saved) { memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved); free(saved); }
                    return 0;
                }
                if (saved) memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved);
                free(saved);
                return m(n->kids[best_i], pos, k, ctx);
            }
        case RN_GROUP: {
            Cont c;
            c.kind = K_CLOSEGROUP;
            c.u.closegroup.idx = n->group_idx;
            c.u.closegroup.start = pos;
            c.next = k;
            return m(n->child, pos, &c, ctx);
        }
        case RN_REP:
            return match_rep(n, pos, k, ctx, 0, 0);
    }
    return 0;
}

static int match_rep(const RNode *rep, int64_t pos, const Cont *k, MCtx *ctx, int reps_done, int blocked) {
    if (reps_done < rep->rep_min) {
        Cont c;
        c.kind = K_STARITER;
        c.u.stariter.rep = rep;
        c.u.stariter.reps_done = reps_done;
        c.u.stariter.start_pos = pos;
        c.next = k;
        return m(rep->child, pos, &c, ctx);
    }
    if (!blocked && (rep->rep_max == -1 || reps_done < rep->rep_max)) {
        Cont c;
        c.kind = K_STARITER;
        c.u.stariter.rep = rep;
        c.u.stariter.reps_done = reps_done;
        c.u.stariter.start_pos = pos;
        c.next = k;
        if (m(rep->child, pos, &c, ctx)) return 1;
    }
    return run_cont(k, pos, ctx);
}

int asnix_regex_match_anchored(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                                int64_t start, int64_t end, int notbol, AsnixRegexGroup *groups_out) {
    MCtx ctx;
    ctx.subject = subject;
    ctx.slen = slen;
    ctx.notbol = notbol;
    ctx.groups = groups_out;
    ctx.nsub = re->nsub;
    for (int64_t i = 0; i < re->nsub; i++) { groups_out[i].so = -1; groups_out[i].eo = -1; }
    ctx.anchored = 1;
    ctx.required_end = end;
    ctx.budget = NULL;
    return m(re->root, start, NULL, &ctx);
}

#define ASNIX_REGEX_SEARCH_BUDGET 300000

int asnix_regex_search(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                        int64_t from_pos, int notbol, int64_t *out_start, int64_t *out_end,
                        AsnixRegexGroup *groups_out) {
    for (int64_t start = from_pos; start <= slen; start++) {
        MCtx ctx;
        ctx.subject = subject;
        ctx.slen = slen;
        ctx.notbol = notbol;
        ctx.groups = NULL;
        ctx.nsub = 0;
        ctx.anchored = 0;
        ctx.found = 0;
        ctx.best_end = -1;
        int64_t budget = ASNIX_REGEX_SEARCH_BUDGET;
        ctx.budget = &budget;
        m(re->root, start, NULL, &ctx);
        if (ctx.found) {
            *out_start = start;
            *out_end = ctx.best_end;
            if (groups_out && re->nsub > 0) asnix_regex_match_anchored(re, subject, slen, start, ctx.best_end, notbol, groups_out);
            return 1;
        }
    }
    return 0;
}

int asnix_regex_needs_custom_engine(const char *p, int64_t len) {
    static const char *std12[] = { "alpha", "digit", "alnum", "upper", "lower", "space",
                                    "blank", "cntrl", "punct", "print", "graph", "xdigit" };
    int in_bracket = 0, bracket_first = 0;
    int64_t i = 0;
    while (i < len) {
        char c = p[i];
        if (!in_bracket) {
            if (c == '\\') { i += (i + 1 < len) ? 2 : 1; continue; }
            if (c == '[') {
                in_bracket = 1;
                bracket_first = 1;
                i++;
                if (i < len && p[i] == '^') i++;
                continue;
            }
            i++;
            continue;
        }
        if (bracket_first && c == ']') { bracket_first = 0; i++; continue; }
        bracket_first = 0;
        if (c == '[' && i + 1 < len && p[i + 1] == '.') {
            return 1;
        }
        if (c == '[' && i + 1 < len && p[i + 1] == '=') {
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == '=' && p[j + 1] == ']')) j++;
            if (j + 1 >= len) { i = len; continue; }
            return 1;
        }
        if (c == '[' && i + 1 < len && p[i + 1] == ':') {
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == ':' && p[j + 1] == ']')) j++;
            if (j + 1 >= len) { i = len; continue; }
            int64_t namelen = j - (i + 2);
            int is_exact_std = 0;
            for (size_t k = 0; k < sizeof std12 / sizeof std12[0]; k++) {
                if ((int64_t)strlen(std12[k]) == namelen && memcmp(p + i + 2, std12[k], (size_t)namelen) == 0) {
                    is_exact_std = 1;
                    break;
                }
            }
            if (!is_exact_std && namelen > 0 && namelen < 16) {
                char lname[16];
                for (int64_t t = 0; t < namelen; t++) lname[t] = (char)tolower((unsigned char)p[i + 2 + t]);
                lname[namelen] = '\0';
                int known = (strcmp(lname, "w") == 0);
                for (size_t k = 0; k < sizeof std12 / sizeof std12[0] && !known; k++)
                    if (strcmp(lname, std12[k]) == 0) known = 1;
                if (known) return 1;
            }
            i = j + 2;
            continue;
        }
        if (c == ']') { in_bracket = 0; i++; continue; }
        i++;
    }
    return 0;
}

