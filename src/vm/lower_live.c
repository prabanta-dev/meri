/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_live.c - which values are alive where, and a register for each.
 *
 * The blocks are laid out in the order they are emitted (the order of the
 * IR if none is given), the instructions numbered one after the other;
 * the parameters of a block are defined at its first number. Liveness is solved
 * on the blocks, the arguments of a jump being uses at its terminator. Each
 * value then gets its pieces: in each block where it is alive, the hull of
 * its points there; exact between blocks, coarser inside one, never
 * smaller. The values, by the start of their first piece, take the lowest
 * register their pieces fit in; two values share a register only if their
 * pieces are disjoint, or if one ends where the other begins: an
 * instruction reads all its operands before it writes its result. Not a str
 * that ends there: it is released from its register after the instruction. A
 * parameter of a block and the arguments passed to it prefer the same register,
 * so that their copy disappears.
 *
 * The copies of the arguments of a jump are written at the terminator of
 * the source, before the target block begins: a register they write may
 * hold a value alive at that point only if the value is dead after the
 * jump (a value alive after it is alive at the start of the target, where
 * the parameter is defined, so their pieces meet), or if it is one of
 * the sources of the same parallel copy.
 */
#include "vm/lower.h"

#include <stdlib.h>
#include <string.h>

bool meri_has_value(const limba_inst *in)
{
    return !(limba_ops[in->op].flags & LIMBA_OPF_NO_RESULT) &&
           in->type != LIMBA_T_VOID;
}

uint32_t meri_value_spans(const limba_func *f, const limba_inst *in,
                          meri_span out[3])
{
    const uint32_t *o = f->operands + in->first;

    switch (limba_ops[in->op].format) {
    case LIMBA_F_ICONST:
    case LIMBA_F_FCONST:
    case LIMBA_F_SCONST:
    case LIMBA_F_TYPED:
    case LIMBA_F_SLOT:
    case LIMBA_F_GADDR:
    case LIMBA_F_FADDR:
    case LIMBA_F_NONE:
    case LIMBA_F_TRAP:
    case LIMBA_F_PARAM:
        return 0;
    case LIMBA_F_BR: /* block, n, n arguments */
        out[0] = (meri_span){o + 2, o[1]};
        return 1;
    case LIMBA_F_CBR: { /* cond, (block, n, args), (block, n, args) */
        uint32_t tn = o[2], en = o[3 + tn + 1];
        out[0] = (meri_span){o, 1};
        out[1] = (meri_span){o + 3, tn};
        out[2] = (meri_span){o + 3 + tn + 2, en};
        return 3;
    }
    case LIMBA_F_SWITCH: /* value, default, n, n x (lo, hi, block) */
        out[0] = (meri_span){o, 1};
        return 1;
    default:
        out[0] = (meri_span){o, in->nops};
        return 1;
    }
}

static const limba_inst *terminator(const limba_func *f, limba_id b)
{
    const limba_block *bl = &f->blocks[b];

    return &f->insts[bl->insts[bl->ninsts - 1]];
}

uint32_t meri_nsuccs(const limba_func *f, limba_id b)
{
    const limba_inst *in = terminator(f, b);

    switch (limba_ops[in->op].format) {
    case LIMBA_F_BR:
        return 1;
    case LIMBA_F_CBR:
        return 2;
    case LIMBA_F_SWITCH:
        return 1 + f->operands[in->first + 2];
    default:
        return 0;
    }
}

limba_id meri_succ(const limba_func *f, limba_id b, uint32_t i)
{
    const limba_inst *in = terminator(f, b);
    const uint32_t *o = f->operands + in->first;

    switch (limba_ops[in->op].format) {
    case LIMBA_F_BR:
        return o[0];
    case LIMBA_F_CBR:
        return i ? o[3 + o[2]] : o[1];
    default: /* switch: the default, then the cases */
        return i ? o[5 + 3 * (i - 1)] : o[1];
    }
}

uint64_t meri_norm(uint64_t v, limba_id t)
{
    unsigned bits = limba_type_bits(t);

    if (bits == 1)
        return v & 1;
    if (bits == 0 || bits >= 64)
        return v;
    return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

/* ---- pointers into strings ---- */

uint32_t meri_str_of_ptr(const limba_func *f, uint32_t v)
{
    const limba_inst *in = &f->insts[v];

    while (in->op == LIMBA_OP_ADDR) {
        v = f->operands[in->first];
        in = &f->insts[v];
    }
    if (in->op == LIMBA_OP_CALLRT && in->imm == LIMBA_RT_STR_PTR &&
        in->nops == 1)
        return f->operands[in->first];
    return LIMBA_NONE;
}

/* the value each value keeps alive where it is used: the str it points
   into, or the base of an addr folded into its loads and stores
   (base_of, may be NULL); LIMBA_NONE for none. NULL if no value keeps
   another (or memory is exhausted: then false) */
static bool keeps_make(const limba_func *f, const uint32_t *base_of,
                       uint32_t **keep)
{
    uint32_t i;
    bool any = base_of != NULL;

    *keep = NULL;
    for (i = 0; i < f->ninsts && !any; i++)
        any = f->insts[i].op == LIMBA_OP_CALLRT &&
              f->insts[i].imm == LIMBA_RT_STR_PTR;
    if (!any)
        return true;
    *keep = malloc(((size_t)f->ninsts + 1) * sizeof(uint32_t));
    if (!*keep)
        return false;
    for (i = 0; i < f->ninsts; i++) {
        (*keep)[i] = meri_str_of_ptr(f, i);
        if ((*keep)[i] == LIMBA_NONE && base_of)
            (*keep)[i] = base_of[i];
    }
    return true;
}

/* ---- liveness ---- */

typedef struct {
    const limba_func *f;
    size_t words;                   /* of a set of values */
    uint64_t *in, *out, *use, *def; /* per block */
    uint32_t *pos;                  /* per value: its number */
    uint32_t *bstart, *bend;        /* per block */
    /* per value: the str it points into (a use of it is a use of that
       str too, so the str lives while the pointer is used), LIMBA_NONE;
       NULL for none at all */
    uint32_t *keep;
} live;

static uint64_t *set_of(uint64_t *sets, size_t words, uint32_t b)
{
    return sets + (size_t)b * words;
}

static void set_add(uint64_t *s, uint32_t v)
{
    s[v / 64] |= 1ull << (v % 64);
}

static bool set_has(const uint64_t *s, uint32_t v)
{
    return s[v / 64] >> (v % 64) & 1;
}

/* hoist: the values defined before block 0 instead of where they are
   (lower.c), or NULL; order: the blocks in the order of the numbers, or
   NULL for the order of the IR */
static bool live_init(live *l, const limba_func *f, const uint8_t *hoist,
                      const uint32_t *base_of, const uint32_t *order)
{
    uint32_t b, k, p = 0, ob;
    size_t n;

    l->f = f;
    l->words = ((size_t)f->ninsts + 63) / 64;
    n = l->words * (f->nblocks ? f->nblocks : 1);
    l->in = calloc(n, sizeof(uint64_t));
    l->out = calloc(n, sizeof(uint64_t));
    l->use = calloc(n, sizeof(uint64_t));
    l->def = calloc(n, sizeof(uint64_t));
    l->pos = calloc((size_t)f->ninsts + 1, sizeof(uint32_t));
    l->bstart = calloc((size_t)f->nblocks + 1, sizeof(uint32_t));
    l->bend = calloc((size_t)f->nblocks + 1, sizeof(uint32_t));
    if (!l->in || !l->out || !l->use || !l->def || !l->pos || !l->bstart ||
        !l->bend || !keeps_make(f, base_of, &l->keep))
        return false;
    if (hoist && f->nblocks)
        for (k = 0; k < f->ninsts; k++)
            if (hoist[k])
                set_add(l->def, k); /* the defs of block 0 */

    for (ob = 0; ob < f->nblocks; ob++) {
        const limba_block *bl;
        uint64_t *use, *def;

        b = order ? order[ob] : ob;
        bl = &f->blocks[b];
        use = set_of(l->use, l->words, b);
        def = set_of(l->def, l->words, b);
        l->bstart[b] = p++;
        for (k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k], s, j;
            const limba_inst *in = &f->insts[id];
            meri_span sp[3];
            uint32_t ns = meri_value_spans(f, in, sp);

            l->pos[id] = k < bl->nparams ? l->bstart[b] : p++;
            if (hoist && hoist[id]) {
                l->pos[id] = 0; /* before everything */
                continue;       /* no operands, defined in block 0 */
            }
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++) {
                    uint32_t u = sp[s].o[j];
                    if (!set_has(def, u))
                        set_add(use, u);
                    if (l->keep && l->keep[u] != LIMBA_NONE &&
                        !set_has(def, l->keep[u]))
                        set_add(use, l->keep[u]);
                }
            if (meri_has_value(in))
                set_add(def, id);
        }
        l->bend[b] = p - 1;
    }
    return true;
}

static void live_solve(live *l)
{
    const limba_func *f = l->f;
    bool changed = true;
    uint32_t b, i, ns;
    size_t w;

    while (changed) {
        changed = false;
        for (b = f->nblocks; b-- > 0;) {
            uint64_t *in = set_of(l->in, l->words, b);
            uint64_t *out = set_of(l->out, l->words, b);
            const uint64_t *use = set_of(l->use, l->words, b);
            const uint64_t *def = set_of(l->def, l->words, b);

            ns = meri_nsuccs(f, b);
            for (i = 0; i < ns; i++) {
                const uint64_t *sin =
                    set_of(l->in, l->words, meri_succ(f, b, i));
                for (w = 0; w < l->words; w++)
                    out[w] |= sin[w];
            }
            for (w = 0; w < l->words; w++) {
                uint64_t x = use[w] | (out[w] & ~def[w]);
                if (x != in[w]) {
                    in[w] = x;
                    changed = true;
                }
            }
        }
    }
}

static void live_free(live *l)
{
    free(l->in);
    free(l->out);
    free(l->use);
    free(l->def);
    free(l->pos);
    free(l->bstart);
    free(l->bend);
    free(l->keep);
}

/* ---- pieces of life, and registers ---- */

#define NOPIECE UINT32_MAX

/* where a value is alive, block by block: in each block of the layout
   the hull of its points there (its definition, its uses, the start of
   the block if alive on entry, the end if alive on exit). Exact between
   blocks, coarse only inside one: a value alive after the end of a loop
   leaves a hole where the loop does not need it */
typedef struct {
    uint32_t start, end, next; /* next: the following piece, or NOPIECE */
} piece;

typedef struct {
    const uint32_t *rep; /* the value whose register a value has */
    const uint8_t *has;  /* of each value: it needs a register */
    uint32_t *lo, *hi;   /* of each value, in the block being walked */
    uint32_t *touched, ntouched;
    piece *pc;
    uint32_t npc, cappc;
    uint32_t *head, *tail; /* of each value: its first and last piece */
} lives;

static void touch(lives *z, uint32_t v, uint32_t p)
{
    v = z->rep[v];
    if (!z->has[v])
        return;
    if (z->lo[v] == NOPIECE) {
        z->lo[v] = z->hi[v] = p;
        z->touched[z->ntouched++] = v;
    } else if (p < z->lo[v]) {
        z->lo[v] = p;
    } else if (p > z->hi[v]) {
        z->hi[v] = p;
    }
}

/* the hulls of the block walked appended to their values: the blocks
   come in the order of the positions, so a piece never begins before the
   last one of its value */
static bool flush(lives *z)
{
    uint32_t i;

    for (i = 0; i < z->ntouched; i++) {
        uint32_t v = z->touched[i], t = z->tail[v];
        if (t != NOPIECE && z->lo[v] <= z->pc[t].end + 1) {
            if (z->hi[v] > z->pc[t].end)
                z->pc[t].end = z->hi[v]; /* blocks side by side: one */
        } else {
            if (z->npc == z->cappc) {
                uint32_t cap = z->cappc ? 2 * z->cappc : 256;
                piece *q = cap > UINT32_MAX / 2
                               ? NULL
                               : realloc(z->pc, (size_t)cap * sizeof(piece));
                if (!q)
                    return false;
                z->pc = q;
                z->cappc = cap;
            }
            z->pc[z->npc] = (piece){z->lo[v], z->hi[v], NOPIECE};
            if (t == NOPIECE)
                z->head[v] = z->npc;
            else
                z->pc[t].next = z->npc;
            z->tail[v] = z->npc++;
        }
        z->lo[v] = NOPIECE;
    }
    z->ntouched = 0;
    return true;
}

/* the pieces of the values a register holds, by start (and by end: two
   of them meet at most at one point, so the ends are in order too) */
typedef struct {
    uint32_t start, end;
    bool counted; /* of a str or a ref: released after its last use */
} seg;

typedef struct {
    seg *s;
    uint32_t n, cap;
} segs;

/* true if value v, defined at def, can take the register whose pieces
   are r: they meet nowhere, or a piece ends at the instruction that
   defines v (it reads its operands before it writes its result). Not a
   str or a ref that ends there: it is released after the instruction.
   Not a parameter of a block: the parameters begin together */
static bool fits(const segs *r, const lives *z, uint32_t v, uint32_t def,
                 bool param)
{
    uint32_t k;

    for (k = z->head[v]; k != NOPIECE; k = z->pc[k].next) {
        uint32_t s = z->pc[k].start, e = z->pc[k].end, lo = 0, hi = r->n;
        while (lo < hi) { /* the first that ends at s or later */
            uint32_t mid = lo + (hi - lo) / 2;
            if (r->s[mid].end < s)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (; lo < r->n && r->s[lo].start <= e; lo++)
            if (r->s[lo].end != s || s != def || r->s[lo].counted || param)
                return false;
    }
    return true;
}

static bool place(segs *r, const lives *z, uint32_t v, bool counted)
{
    uint32_t k;

    for (k = z->head[v]; k != NOPIECE; k = z->pc[k].next) {
        seg g = {z->pc[k].start, z->pc[k].end, counted};
        uint32_t lo = 0, hi = r->n;
        if (r->n == r->cap) {
            uint32_t cap = r->cap ? 2 * r->cap : 8;
            seg *q = cap > UINT32_MAX / 2
                         ? NULL
                         : realloc(r->s, (size_t)cap * sizeof(seg));
            if (!q)
                return false;
            r->s = q;
            r->cap = cap;
        }
        while (lo < hi) { /* after those that begin before it */
            uint32_t mid = lo + (hi - lo) / 2;
            if (r->s[mid].start < g.start ||
                (r->s[mid].start == g.start && r->s[mid].end <= g.end))
                lo = mid + 1;
            else
                hi = mid;
        }
        memmove(r->s + lo + 1, r->s + lo, (r->n - lo) * sizeof(seg));
        r->s[lo] = g;
        r->n++;
    }
    return true;
}

/* the comparisons whose only use is the cbr right after them */
static void find_fused(const limba_func *f, uint8_t *fused)
{
    uint32_t *uses = calloc((size_t)f->ninsts + 1, sizeof(uint32_t));
    uint32_t b, i, s, j;

    if (!uses)
        return; /* nothing fused: slower, not wrong */
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t ns = meri_value_spans(f, &f->insts[i], sp);
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++)
                uses[sp[s].o[j]]++;
    }
    for (b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        uint32_t t, c;
        const limba_inst *term;

        if (bl->ninsts < 2)
            continue;
        t = bl->insts[bl->ninsts - 1];
        c = bl->insts[bl->ninsts - 2];
        term = &f->insts[t];
        if (term->op == LIMBA_OP_CBR &&
            (f->insts[c].op == LIMBA_OP_ICMP ||
             f->insts[c].op == LIMBA_OP_FCMP) &&
            uses[c] == 1 && f->operands[term->first] == c &&
            bl->ninsts - 2 >= bl->nparams)
            fused[c] = 1;
    }
    free(uses);
}

/* the values a value would like to share its register with: the
   parameters of a block and the arguments of the jumps to it, both ways;
   hint[first[v] .. first[v + 1]) */
typedef struct {
    uint32_t *first, *hint;
} hints;

/* call f for each (parameter, argument) pair of every jump */
static void each_pair(const limba_func *f,
                      void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    uint32_t b, e, j;

    for (b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        const limba_inst *t = &f->insts[bl->insts[bl->ninsts - 1]];
        const uint32_t *o = f->operands + t->first;
        uint32_t k[2], nk = 0;
        if (t->op == LIMBA_OP_BR)
            k[nk++] = 0;
        else if (t->op == LIMBA_OP_CBR)
            k[nk++] = 1, k[nk++] = 3 + o[2];
        for (e = 0; e < nk; e++) {
            const limba_block *to = &f->blocks[o[k[e]]];
            for (j = 0; j < o[k[e] + 1]; j++)
                fn(ctx, to->insts[j], o[k[e] + 2 + j]);
        }
    }
}

static void count_pair(void *ctx, uint32_t p, uint32_t a)
{
    uint32_t *deg = ctx;

    deg[p]++;
    deg[a]++;
}

typedef struct {
    uint32_t *at, *hint;
} filling;

static void fill_pair(void *ctx, uint32_t p, uint32_t a)
{
    filling *fl = ctx;

    fl->hint[fl->at[p]++] = a;
    fl->hint[fl->at[a]++] = p;
}

static bool hints_make(const limba_func *f, hints *h)
{
    size_t n = (size_t)f->ninsts;
    uint32_t *at = calloc(n + 1, sizeof(uint32_t)), i;
    filling fl;

    h->first = calloc(n + 1, sizeof(uint32_t));
    h->hint = NULL;
    if (!at || !h->first) {
        free(at);
        return false;
    }
    each_pair(f, count_pair, at); /* the degrees, for now */
    for (i = 0; i < f->ninsts; i++)
        h->first[i + 1] = h->first[i] + at[i];
    h->hint = malloc(((size_t)h->first[n] + 1) * sizeof(uint32_t));
    if (!h->hint) {
        free(at);
        return false;
    }
    memcpy(at, h->first, (n + 1) * sizeof(uint32_t));
    fl = (filling){at, h->hint};
    each_pair(f, fill_pair, &fl);
    free(at);
    return true;
}

static void hints_free(hints *h)
{
    free(h->first);
    free(h->hint);
}

typedef struct {
    uint32_t start, value;
} entry;

static int by_start(const void *x, const void *y)
{
    const entry *a = x, *b = y;

    if (a->start != b->start)
        return a->start < b->start ? -1 : 1;
    return a->value < b->value ? -1 : a->value > b->value;
}

/* the pieces of registers 0 .. need - 1 at hand, the new ones empty */
static bool regs_grow(segs **rs, uint32_t *n, uint32_t need)
{
    segs *q;

    if (need <= *n)
        return true;
    q = realloc(*rs, (size_t)need * sizeof(segs));
    if (!q)
        return false;
    memset(q + *n, 0, (size_t)(need - *n) * sizeof(segs));
    *rs = q;
    *n = need;
    return true;
}

static bool is_str(const limba_func *f, uint32_t v);

/* true if a value related to p has a register: p would rather have it */
static bool related_given(const hints *h, const meri_alloc *a,
                          const uint32_t *rep, uint32_t p)
{
    uint32_t k;

    for (k = h->first[p]; k < h->first[p + 1]; k++)
        if (a->reg[rep[h->hint[k]]] != MERI_NOREG)
            return true;
    return false;
}

/* true if the pieces of v and w meet nowhere, not even at a point */
static bool apart(const lives *z, uint32_t v, uint32_t w)
{
    uint32_t x = z->head[v], y = z->head[w];

    while (x != NOPIECE && y != NOPIECE) {
        if (z->pc[x].end < z->pc[y].start)
            x = z->pc[x].next;
        else if (z->pc[y].end < z->pc[x].start)
            y = z->pc[y].next;
        else
            return false;
    }
    return true;
}

bool meri_alloc_regs(const limba_func *f, uint32_t max, uint32_t skip,
                     uint32_t nskip, const meri_fusion *fu, meri_alloc *a)
{
    live l = {0};
    lives z = {0};
    size_t ni = (size_t)f->ninsts + 1, w;
    uint32_t *rep = malloc(ni * sizeof(uint32_t));
    uint8_t *has = calloc(ni, 1);
    entry *ord = malloc(ni * sizeof(entry));
    segs *rs = NULL; /* of each register */
    uint32_t nrs = 0, n = 0, ob, b, i, k, s, j;
    hints h = {0};
    bool ok = false;

    memset(a, 0, sizeof(*a));
    a->reg = malloc(ni * sizeof(uint16_t));
    a->fused = calloc(ni, 1);
    z.rep = rep;
    z.has = has;
    z.lo = malloc(ni * sizeof(uint32_t));
    z.hi = malloc(ni * sizeof(uint32_t));
    z.touched = malloc(ni * sizeof(uint32_t));
    z.head = malloc(ni * sizeof(uint32_t));
    z.tail = malloc(ni * sizeof(uint32_t));
    if (!a->reg || !a->fused || !rep || !has || !ord || !z.lo || !z.hi ||
        !z.touched || !z.head || !z.tail || max > MERI_NOREG ||
        !live_init(&l, f, fu ? fu->hoist : NULL, fu ? fu->base_of : NULL,
                   fu ? fu->order : NULL) ||
        !hints_make(f, &h))
        goto done;
    live_solve(&l);
    find_fused(f, a->fused);

    for (i = 0; i < f->ninsts; i++) {
        a->reg[i] = MERI_NOREG;
        rep[i] = i;
        z.lo[i] = z.head[i] = z.tail[i] = NOPIECE;
        if (fu && fu->absorbed[i])
            a->fused[i] = 1; /* no register, as a fused comparison */
        has[i] = meri_has_value(&f->insts[i]) && !a->fused[i];
    }
    /* a copy lives in the register of its source: the points of both
       are those of the source */
    if (fu && fu->alias)
        for (i = 0; i < f->ninsts; i++) {
            uint32_t r0 = i;
            if (fu->alias[i] == LIMBA_NONE || !has[i])
                continue;
            while (fu->alias[r0] != LIMBA_NONE)
                r0 = fu->alias[r0];
            if (has[r0]) /* else it is given its own */
                rep[i] = r0;
        }

    /* the pieces, block by block in the layout. The operands of an
       absorbed instruction are read at its anchor, in the same block
       (lower.c fuses only inside a block) */
    for (ob = 0; ob < f->nblocks; ob++) {
        const limba_block *bl;
        const uint64_t *in, *out;

        b = fu && fu->order ? fu->order[ob] : ob;
        bl = &f->blocks[b];
        if (ob == 0 && fu && fu->hoist) /* defined before block 0 */
            for (i = 0; i < f->ninsts; i++)
                if (fu->hoist[i])
                    touch(&z, i, 0);
        in = set_of(l.in, l.words, b);
        out = set_of(l.out, l.words, b);
        for (w = 0; w < l.words; w++) {
            uint64_t x = in[w], y = out[w];
            for (; x; x &= x - 1)
                touch(&z, (uint32_t)(w * 64) + (uint32_t)__builtin_ctzll(x),
                      l.bstart[b]);
            for (; y; y &= y - 1)
                touch(&z, (uint32_t)(w * 64) + (uint32_t)__builtin_ctzll(y),
                      l.bend[b]);
        }
        for (k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k], where, ns;
            meri_span sp[3];
            if (fu && fu->hoist && fu->hoist[id])
                continue;
            touch(&z, id, l.pos[id]);
            where =
                l.pos[fu && fu->anchor[id] != LIMBA_NONE ? fu->anchor[id] : id];
            ns = meri_value_spans(f, &f->insts[id], sp);
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++) {
                    uint32_t u = sp[s].o[j];
                    touch(&z, u, where);
                    if (l.keep && l.keep[u] != LIMBA_NONE)
                        touch(&z, l.keep[u], where);
                }
        }
        if (!flush(&z))
            goto done;
    }
    for (i = 0; i < f->ninsts; i++)
        if (has[i] && rep[i] == i && z.head[i] != NOPIECE)
            ord[n++] = (entry){z.pc[z.head[i]].start, i};

    /* the parameters of the entry block take 0 .. n - 1, the convention
       of a call */
    if (f->nblocks) {
        const limba_block *e = &f->blocks[0];
        if (e->nparams > max || e->nparams > skip ||
            !regs_grow(&rs, &nrs, e->nparams))
            goto done;
        for (k = 0; k < e->nparams; k++) {
            uint32_t v = e->insts[k];
            a->reg[v] = (uint16_t)k;
            if (!place(&rs[k], &z, v, is_str(f, v)))
                goto done;
            if (k + 1 > a->nregs)
                a->nregs = k + 1;
        }
    }
    qsort(ord, n, sizeof(entry), by_start);
    for (i = 0; i < n; i++) {
        uint32_t v = ord[i].value, r = max, def = l.pos[v], mate = NOPIECE;
        bool param = f->insts[v].op == LIMBA_OP_PARAM;

        if (a->reg[v] != MERI_NOREG)
            continue; /* an entry parameter, given already */
        /* a register a related value has, if it fits: one of its own,
           else one of the values related to a related value that has no
           register yet (the other arguments of the same parameter);
           else the lowest */
        for (k = h.first[v]; k < h.first[v + 1] && r == max; k++) {
            uint16_t q = a->reg[rep[h.hint[k]]];
            if (q != MERI_NOREG && q < max &&
                (q >= nrs || fits(&rs[q], &z, v, def, param)))
                r = q;
        }
        for (k = h.first[v]; k < h.first[v + 1] && r == max; k++) {
            uint32_t p = rep[h.hint[k]], m;
            if (a->reg[p] != MERI_NOREG)
                continue;
            for (m = h.first[p]; m < h.first[p + 1] && r == max; m++) {
                uint16_t q = a->reg[rep[h.hint[m]]];
                if (q != MERI_NOREG && q < max &&
                    (q >= nrs || fits(&rs[q], &z, v, def, param)))
                    r = q;
            }
        }
        /* no related value has a register (if one has, its register did
           not fit, and a copy is left to its edge): the first related one
           that v never meets, and whose related values have no register
           either, takes with v the lowest register both fit */
        for (k = h.first[v]; k < h.first[v + 1] && r == max; k++) {
            uint32_t p = rep[h.hint[k]];
            if (a->reg[p] != MERI_NOREG) {
                mate = NOPIECE;
                break;
            }
            if (mate == NOPIECE && p != v && z.head[p] != NOPIECE &&
                !related_given(&h, a, rep, p) && apart(&z, v, p))
                mate = p;
        }
        for (k = 0; k < max && r == max; k++)
            if ((k < skip || k >= skip + nskip) &&
                (k >= nrs || (fits(&rs[k], &z, v, def, param) &&
                              (mate == NOPIECE ||
                               fits(&rs[k], &z, mate, l.pos[mate],
                                    f->insts[mate].op == LIMBA_OP_PARAM)))))
                r = k;
        if (mate != NOPIECE && r != max) {
            if (!regs_grow(&rs, &nrs, r + 1) ||
                !place(&rs[r], &z, mate, is_str(f, mate)))
                goto done;
            a->reg[mate] = (uint16_t)r; /* nregs: with v, below */
        }
        if (r == max || !regs_grow(&rs, &nrs, r + 1) ||
            !place(&rs[r], &z, v, is_str(f, v)))
            goto done;
        a->reg[v] = (uint16_t)r;
        if (r + 1 > a->nregs)
            a->nregs = r + 1;
    }
    for (i = 0; i < f->ninsts; i++)
        if (rep[i] != i)
            a->reg[i] = a->reg[rep[i]];
    ok = true;
done:
    live_free(&l);
    for (k = 0; k < nrs; k++)
        free(rs[k].s);
    free(rs);
    free(rep);
    free(has);
    free(ord);
    free(z.lo);
    free(z.hi);
    free(z.touched);
    free(z.head);
    free(z.tail);
    free(z.pc);
    hints_free(&h);
    return ok;
}

/* ---- the deaths of the strings ---- */

/* a counted handle (progetto_ir.md § 11c, § 11d): a str, or the ref of a
   BigInt; both die, and are released, the same way */
static bool is_str(const limba_func *f, uint32_t v)
{
    return f->insts[v].type == LIMBA_T_STR || f->insts[v].type == LIMBA_T_REF;
}

/* v appended to list l, the last list of the pool */
static bool plan_add(meri_strplan *p, meri_list *l, uint32_t v)
{
    uint32_t i;

    for (i = 0; i < l->n; i++)
        if (p->pool[l->first + i] == v)
            return true;
    if (p->npool == p->cappool) {
        uint32_t cap = p->cappool ? 2 * p->cappool : 64;
        uint32_t *q = cap > UINT32_MAX / 2
                          ? NULL
                          : realloc(p->pool, (size_t)cap * sizeof(uint32_t));
        if (!q)
            return false;
        p->pool = q;
        p->cappool = cap;
    }
    p->pool[p->npool++] = v;
    l->n++;
    return true;
}

static void begin(meri_strplan *p, meri_list *l)
{
    l->first = p->npool;
    l->n = 0;
}

bool meri_str_plan(const limba_func *f, meri_strplan *p)
{
    live l = {0};
    uint64_t *now = NULL; /* alive at the point being walked */
    uint32_t b, i, nedges = 0, e;
    size_t w;
    bool ok = false;

    memset(p, 0, sizeof(*p));
    for (b = 0; b < f->nblocks; b++)
        nedges += meri_nsuccs(f, b);
    p->after = calloc((size_t)f->ninsts + 1, sizeof(meri_list));
    p->start = calloc((size_t)f->nblocks + 1, sizeof(meri_list));
    p->at_ret = calloc((size_t)f->nblocks + 1, sizeof(meri_list));
    p->edge_at = calloc((size_t)f->nblocks + 1, sizeof(uint32_t));
    p->edge = calloc((size_t)nedges + 1, sizeof(meri_list));
    if (!p->after || !p->start || !p->at_ret || !p->edge_at || !p->edge ||
        !live_init(&l, f, NULL, NULL, NULL))
        goto done;
    live_solve(&l);
    now = calloc(l.words, sizeof(uint64_t));
    if (!now)
        goto done;

    for (b = 0, e = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        uint32_t t = bl->insts[bl->ninsts - 1], ns, s, j, k;
        const limba_inst *term = &f->insts[t];
        meri_span sp[3];

        /* alive just before the terminator: alive after it, and its uses */
        memcpy(now, set_of(l.out, l.words, b), l.words * sizeof(uint64_t));
        ns = meri_value_spans(f, term, sp);
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++) {
                set_add(now, sp[s].o[j]);
                if (l.keep && l.keep[sp[s].o[j]] != LIMBA_NONE)
                    set_add(now, l.keep[sp[s].o[j]]);
            }

        p->edge_at[b] = e;
        if (term->op == LIMBA_OP_RET) {
            uint32_t r = term->nops ? f->operands[term->first] : LIMBA_NONE;
            begin(p, &p->at_ret[b]);
            for (i = 0; i < f->ninsts; i++)
                if (set_has(now, i) && is_str(f, i) && i != r &&
                    !plan_add(p, &p->at_ret[b], i))
                    goto done;
        }
        for (k = 0; k < meri_nsuccs(f, b); k++, e++) {
            const uint64_t *in = set_of(l.in, l.words, meri_succ(f, b, k));
            begin(p, &p->edge[e]);
            for (i = 0; i < f->ninsts; i++)
                if (set_has(now, i) && !set_has(in, i) && is_str(f, i) &&
                    !plan_add(p, &p->edge[e], i))
                    goto done;
        }

        /* the other instructions, backwards */
        for (k = bl->ninsts - 1; k-- > bl->nparams;) {
            uint32_t id = bl->insts[k];
            const limba_inst *in = &f->insts[id];
            begin(p, &p->after[id]);
            if (meri_has_value(in)) {
                if (is_str(f, id) && !set_has(now, id) &&
                    !plan_add(p, &p->after[id], id))
                    goto done;
                now[id / 64] &= ~(1ull << (id % 64));
            }
            ns = meri_value_spans(f, in, sp);
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++) {
                    uint32_t u = sp[s].o[j];
                    uint32_t kv = l.keep ? l.keep[u] : LIMBA_NONE;
                    if (is_str(f, u) && !set_has(now, u) &&
                        !plan_add(p, &p->after[id], u))
                        goto done;
                    if (kv != LIMBA_NONE && !set_has(now, kv) &&
                        !plan_add(p, &p->after[id], kv))
                        goto done;
                }
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++) {
                    set_add(now, sp[s].o[j]);
                    if (l.keep && l.keep[sp[s].o[j]] != LIMBA_NONE)
                        set_add(now, l.keep[sp[s].o[j]]);
                }
        }
        begin(p, &p->start[b]);
        for (k = 0; k < bl->nparams; k++) {
            uint32_t v = bl->insts[k];
            if (is_str(f, v) && !set_has(now, v) &&
                !plan_add(p, &p->start[b], v))
                goto done;
        }
        for (w = 0; w < l.words; w++)
            now[w] = 0;
    }
    ok = true;
done:
    free(now);
    live_free(&l);
    if (!ok)
        meri_strplan_free(p);
    return ok;
}

void meri_strplan_free(meri_strplan *p)
{
    free(p->pool);
    free(p->after);
    free(p->start);
    free(p->edge_at);
    free(p->edge);
    free(p->at_ret);
    memset(p, 0, sizeof(*p));
}

void meri_alloc_free(meri_alloc *a)
{
    free(a->reg);
    free(a->fused);
    a->reg = NULL;
    a->fused = NULL;
}
