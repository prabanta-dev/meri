/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_live.c - which values are alive where, and a register for each.
 *
 * The blocks are laid out in the order of the IR, the instructions
 * numbered one after the other; the parameters of a block are defined at
 * its first number. Liveness is solved on the blocks, the arguments of a
 * jump being uses at its terminator. Each value then gets one interval,
 * the hull of every point where it is alive: coarser than the exact
 * ranges, never smaller. Linear scan gives registers from the lowest free
 * one; two values share a register only if their intervals are disjoint,
 * or if one ends where the other begins: an instruction reads all its
 * operands before it writes its result. Not a str that ends there: it is
 * released from its register after the instruction. A parameter of a
 * block and the arguments passed to it prefer the same register, so that
 * their copy disappears.
 *
 * The copies of the arguments of a jump are written at the terminator of
 * the source, before the target block begins: a register they write may
 * hold a value alive at that point only if the value is dead after the
 * jump (a value alive after it is alive at the start of the target, where
 * the parameter is defined, so their intervals meet), or if it is one of
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

/* ---- liveness ---- */

typedef struct {
    const limba_func *f;
    size_t words;                   /* of a set of values */
    uint64_t *in, *out, *use, *def; /* per block */
    uint32_t *pos;                  /* per value: its number */
    uint32_t *bstart, *bend;        /* per block */
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

static bool live_init(live *l, const limba_func *f)
{
    uint32_t b, k, p = 0;
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
        !l->bend)
        return false;

    for (b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        uint64_t *use = set_of(l->use, l->words, b);
        uint64_t *def = set_of(l->def, l->words, b);

        l->bstart[b] = p++;
        for (k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k], s, j;
            const limba_inst *in = &f->insts[id];
            meri_span sp[3];
            uint32_t ns = meri_value_spans(f, in, sp);

            l->pos[id] = k < bl->nparams ? l->bstart[b] : p++;
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++)
                    if (!set_has(def, sp[s].o[j]))
                        set_add(use, sp[s].o[j]);
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
}

/* ---- intervals and registers ---- */

typedef struct {
    uint32_t start, end, value;
} interval;

static int by_start(const void *x, const void *y)
{
    const interval *a = x, *b = y;

    if (a->start != b->start)
        return a->start < b->start ? -1 : 1;
    return a->value < b->value ? -1 : a->value > b->value;
}

static void widen(interval *iv, uint32_t p)
{
    if (p < iv->start)
        iv->start = p;
    if (p > iv->end)
        iv->end = p;
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

bool meri_alloc_regs(const limba_func *f, uint32_t max, uint32_t skip,
                     uint32_t nskip, const meri_fusion *fu, meri_alloc *a)
{
    live l = {0};
    interval *iv = NULL, *active = NULL;
    uint32_t *at = NULL; /* value -> index in iv */
    uint32_t n = 0, nactive = 0, b, i, k, s, j;
    uint64_t *freeset = NULL; /* a bit for each register free */
    size_t nwords = ((size_t)max + 63) / 64, wi;
    hints h = {0};
    bool ok = false;

    memset(a, 0, sizeof(*a));
    a->reg = malloc(((size_t)f->ninsts + 1) * sizeof(uint16_t));
    a->fused = calloc((size_t)f->ninsts + 1, 1);
    iv = malloc(((size_t)f->ninsts + 1) * sizeof(interval));
    active = malloc(((size_t)f->ninsts + 1) * sizeof(interval));
    at = malloc(((size_t)f->ninsts + 1) * sizeof(uint32_t));
    freeset = calloc(nwords + 1, sizeof(uint64_t));
    if (!a->reg || !a->fused || !iv || !active || !at || !freeset ||
        max > MERI_NOREG || !live_init(&l, f) || !hints_make(f, &h))
        goto done;
    live_solve(&l);
    find_fused(f, a->fused);

    for (i = 0; i < f->ninsts; i++) {
        a->reg[i] = MERI_NOREG;
        at[i] = UINT32_MAX;
        if (fu && fu->absorbed[i])
            a->fused[i] = 1; /* no register, as a fused comparison */
        if (meri_has_value(&f->insts[i]) && !a->fused[i]) {
            at[i] = n;
            iv[n++] = (interval){l.pos[i], l.pos[i], i};
        }
    }
    /* uses, and the blocks where a value is alive on entry or exit */
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t ns = meri_value_spans(f, &f->insts[i], sp);
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++)
                if (at[sp[s].o[j]] != UINT32_MAX)
                    widen(
                        &iv[at[sp[s].o[j]]],
                        l.pos[fu && fu->anchor[i] != LIMBA_NONE ? fu->anchor[i]
                                                                : i]);
    }
    for (b = 0; b < f->nblocks; b++) {
        const uint64_t *in = set_of(l.in, l.words, b);
        const uint64_t *out = set_of(l.out, l.words, b);
        for (i = 0; i < f->ninsts; i++) {
            if (at[i] == UINT32_MAX)
                continue;
            if (set_has(in, i))
                widen(&iv[at[i]], l.bstart[b]);
            if (set_has(out, i))
                widen(&iv[at[i]], l.bend[b]);
        }
    }

    /* the parameters of the entry block take 0 .. n - 1, the convention
       of a call; they begin at 0 and come first in the order */
    for (k = 0; k < max; k++)
        if (k < skip || k >= skip + nskip)
            freeset[k / 64] |= 1ull << (k % 64);
    if (f->nblocks) {
        const limba_block *e = &f->blocks[0];
        if (e->nparams > max || e->nparams > skip)
            goto done;
        for (k = 0; k < e->nparams; k++) {
            uint32_t v = e->insts[k];
            a->reg[v] = (uint16_t)k;
            freeset[k / 64] &= ~(1ull << (k % 64));
            active[nactive++] = iv[at[v]];
            if (k + 1 > a->nregs)
                a->nregs = k + 1;
        }
    }
    qsort(iv, n, sizeof(interval), by_start);
    for (i = 0; i < n; i++) {
        uint32_t v = iv[i].value, r;

        if (a->reg[v] != MERI_NOREG)
            continue; /* an entry parameter, given already */
        for (k = 0; k < nactive;) {
            uint32_t av = active[k].value;
            /* disjoint, or ending at the instruction that defines the
               new one: its start must be its definition (not a point
               where it is only alive, before a later definition in the
               layout), and not the head of a block, where parameters
               begin together */
            if (active[k].end < iv[i].start ||
                (active[k].end == iv[i].start && iv[i].start == l.pos[v] &&
                 f->insts[av].type != LIMBA_T_STR &&
                 f->insts[v].op != LIMBA_OP_PARAM)) {
                r = a->reg[active[k].value];
                freeset[r / 64] |= 1ull << (r % 64);
                active[k] = active[--nactive];
            } else {
                k++;
            }
        }
        /* a register a related value has, if free; else the lowest */
        r = max;
        for (k = h.first[v]; k < h.first[v + 1] && r == max; k++) {
            uint16_t q = a->reg[h.hint[k]];
            if (q != MERI_NOREG && q < max && freeset[q / 64] >> (q % 64) & 1)
                r = q;
        }
        if (r == max) {
            for (wi = 0; wi < nwords && !freeset[wi]; wi++)
                ;
            if (wi == nwords)
                goto done;
            r = (uint32_t)(wi * 64) + (uint32_t)__builtin_ctzll(freeset[wi]);
            if (r >= max)
                goto done;
        }
        freeset[r / 64] &= ~(1ull << (r % 64));
        a->reg[v] = (uint16_t)r;
        active[nactive++] = iv[i];
        if (r + 1 > a->nregs)
            a->nregs = r + 1;
    }
    ok = true;
done:
    live_free(&l);
    free(iv);
    free(active);
    free(at);
    free(freeset);
    hints_free(&h);
    return ok;
}

/* ---- the deaths of the strings ---- */

static bool is_str(const limba_func *f, uint32_t v)
{
    return f->insts[v].type == LIMBA_T_STR;
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
        !live_init(&l, f))
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
            for (j = 0; j < sp[s].n; j++)
                set_add(now, sp[s].o[j]);

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
                    if (is_str(f, u) && !set_has(now, u) &&
                        !plan_add(p, &p->after[id], u))
                        goto done;
                }
            for (s = 0; s < ns; s++)
                for (j = 0; j < sp[s].n; j++)
                    set_add(now, sp[s].o[j]);
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
