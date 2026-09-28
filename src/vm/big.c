/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * big.c - BigInt on mini-gmp (big.h). The checks the IR owes a call (a
 * divisor not 0, a real finite and integral) come before it; were one
 * missing, the call stops the run with a trap rather than let mini-gmp
 * abort it.
 *
 * mini-gmp allocates through the functions here: every byte counts
 * against the budget of the run, and past it (or when malloc fails) they
 * jump back to meri_big_call, which gives the trap NOMEM. A number being
 * made stays listed, and is freed with the others at the end of the run.
 */
#include "vm/big.h"

#include "limba/val.h"
#include "third_party/mini-gmp/mini-gmp.h"

#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct meri_big {
    uint64_t rc; /* first, as in a str: meri_str_retain */
    size_t slot; /* where the run lists it */
    mpz_t z;
};
typedef struct meri_big meri_big;

_Static_assert(offsetof(meri_big, rc) == 0 && offsetof(meri_str, rc) == 0,
               "a reference count first in every counted value");

/* the run whose numbers mini-gmp is working on, and where to jump when
   its memory runs out (NULL outside meri_big_call) */
static _Thread_local meri_state *cur;
static _Thread_local jmp_buf *cur_jmp;

static void out_of_memory(void)
{
    if (cur_jmp)
        longjmp(*cur_jmp, 1);
    abort(); /* mini-gmp allocates only within meri_big_call */
}

static void *gmp_alloc(size_t n)
{
    void *p;

    if (!cur || !meri_state_take(cur, n))
        out_of_memory();
    p = malloc(n);
    if (!p) {
        meri_state_give(cur, n);
        out_of_memory();
    }
    return p;
}

static void *gmp_realloc(void *old, size_t on, size_t nn)
{
    void *p;

    if (nn > on && (!cur || !meri_state_take(cur, nn - on)))
        out_of_memory();
    p = realloc(old, nn);
    if (!p) {
        if (nn > on)
            meri_state_give(cur, nn - on);
        out_of_memory();
    }
    if (nn < on)
        meri_state_give(cur, on - nn);
    return p;
}

static void gmp_free(void *p, size_t n)
{
    free(p);
    if (cur)
        meri_state_give(cur, n);
}

static void bind(meri_state *s)
{
    if (cur != s) {
        cur = s;
        mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
    }
}

/* the number a value holds, 0 the number 0 */
static mpz_srcptr num(uint64_t v)
{
    static const __mpz_struct zero; /* 0 limbs: 0, never written */

    return v ? ((const meri_big *)(uintptr_t)v)->z : &zero;
}

/* a new number of the run, 0, with one reference (the caller's); NULL
   when memory is exhausted */
static meri_big *fresh(meri_state *s)
{
    meri_big *x;

    if (s->nbigs == s->capbigs) {
        size_t cap = s->capbigs ? 2 * s->capbigs : 64;
        struct meri_big **b = cap > SIZE_MAX / sizeof(*b)
                                  ? NULL
                                  : realloc(s->bigs, cap * sizeof(*b));
        if (!b)
            return NULL;
        s->bigs = b;
        s->capbigs = cap;
    }
    if (!meri_state_take(s, sizeof(meri_big)))
        return NULL;
    x = malloc(sizeof(*x));
    if (!x) {
        meri_state_give(s, sizeof(meri_big));
        return NULL;
    }
    x->rc = 1;
    x->slot = s->nbigs;
    mpz_init(x->z);
    s->bigs[s->nbigs++] = x;
    return x;
}

void meri_big_release(meri_state *s, uint64_t v)
{
    meri_big *x = (meri_big *)(uintptr_t)v, *last;

    if (!x || x->rc == MERI_RC_IMMORTAL || --x->rc)
        return;
    bind(s);
    last = s->bigs[--s->nbigs];
    s->bigs[x->slot] = last;
    last->slot = x->slot;
    mpz_clear(x->z);
    free(x);
    meri_state_give(s, sizeof(meri_big));
}

uint64_t meri_big_alive(const meri_state *s)
{
    uint64_t n = 0;
    size_t i;

    for (i = 0; i < s->nbigs; i++)
        n += s->bigs[i]->rc != MERI_RC_IMMORTAL;
    return n;
}

void meri_big_free_all(meri_state *s)
{
    size_t i;

    bind(s);
    for (i = 0; i < s->nbigs; i++) {
        mpz_clear(s->bigs[i]->z);
        free(s->bigs[i]);
    }
    free(s->bigs);
    s->bigs = NULL;
    s->nbigs = s->capbigs = 0;
    cur = NULL;
}

/* ---- to the reals: one rounding, to nearest even, straight to the type */

/* bits [at, at + k) of |z|, k <= 64 */
static uint64_t bits_at(mpz_srcptr z, size_t at, unsigned k)
{
    size_t i = at / 64, sh = at % 64;
    uint64_t lo = mpz_getlimbn(z, (mp_size_t)i) >> sh;

    if (sh && i + 1 < mpz_size(z))
        lo |= (uint64_t)mpz_getlimbn(z, (mp_size_t)(i + 1)) << (64 - sh);
    return k == 64 ? lo : lo & (((uint64_t)1 << k) - 1);
}

/* some bit of |z| below bit at is 1 */
static bool sticky(mpz_srcptr z, size_t at)
{
    size_t i;

    for (i = 0; i < at / 64; i++)
        if (mpz_getlimbn(z, (mp_size_t)i))
            return true;
    return at % 64 && (mpz_getlimbn(z, (mp_size_t)(at / 64)) &
                       (((uint64_t)1 << (at % 64)) - 1)) != 0;
}

/* |z| rounded to p bits (53 for f64, 24 for f32), as m * 2^e with m
   below 2^p... or 2^p itself when the rounding carries; z not 0 */
static void round_bits(mpz_srcptr z, unsigned p, uint64_t *m, long *e)
{
    size_t n = mpz_sizeinbase(z, 2);

    if (n <= p) {
        *m = bits_at(z, 0, (unsigned)n);
        *e = 0;
        return;
    }
    /* p bits and the one after them (the guard); sticky below it */
    *m = bits_at(z, n - p - 1, p + 1);
    {
        bool guard = *m & 1, rest = sticky(z, n - p - 1);
        *m >>= 1;
        if (guard && (rest || (*m & 1)))
            (*m)++;
    }
    *e = (long)(n - p);
}

static double to_f64(mpz_srcptr z)
{
    uint64_t m;
    long e;
    double d;

    if (!mpz_sgn(z))
        return 0.0;
    round_bits(z, 53, &m, &e);
    /* m <= 2^53: exact; past DBL_MAX ldexp gives the infinity */
    d = e > 2000 ? HUGE_VAL : ldexp((double)m, (int)e);
    return mpz_sgn(z) < 0 ? -d : d;
}

static float to_f32(mpz_srcptr z)
{
    uint64_t m;
    long e;
    double d;

    if (!mpz_sgn(z))
        return 0.0f;
    round_bits(z, 24, &m, &e);
    /* m <= 2^24 and e small: d is a float or past FLT_MAX, where the
       conversion gives the infinity */
    d = e > 200 ? HUGE_VAL : ldexp((double)m, (int)e);
    return (float)(mpz_sgn(z) < 0 ? -d : d);
}

/* ---- the functions */

static uint64_t fbits(double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

static uint64_t f32bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static void put64(uint64_t p, uint64_t v)
{
    memcpy((void *)(uintptr_t)MERI_ADDR(p), &v, sizeof(v));
}

/* the text of z in decimal, in memory of its own (free it); NULL when
   memory is exhausted */
static char *text(mpz_srcptr z)
{
    char *b = malloc(mpz_sizeinbase(z, 10) + 2);

    if (b)
        mpz_get_str(b, 10, z);
    return b;
}

static int trap(meri_state *s, int64_t code)
{
    meri_rt_trap(s, code);
    return 0;
}

/* a power with base b and an unsigned exponent n: exact for 0, 1, -1;
   refused before it is made if its bits cannot fit the budget left */
static int power(meri_state *s, meri_big *r, mpz_srcptr b, uint64_t n)
{
    uint64_t bits, room;

    if (!mpz_sgn(b)) {
        mpz_set_ui(r->z, n == 0); /* 0 ** 0 is 1 */
        return 1;
    }
    if (!mpz_cmpabs_ui(b, 1)) {
        mpz_set_si(r->z, mpz_sgn(b) < 0 && (n & 1) ? -1 : 1);
        return 1;
    }
    room = s->budget - s->used;
    if (__builtin_mul_overflow((uint64_t)(mpz_sizeinbase(b, 2) - 1), n,
                               &bits) ||
        bits / 8 > room)
        return trap(s, LIMBA_TRAP_NOMEM);
    mpz_pow_ui(r->z, b, (unsigned long)n);
    return 1;
}

/* the value of the new number x in a[0] */
static int give(uint64_t *a, meri_big *x)
{
    a[0] = (uint64_t)(uintptr_t)x;
    return 1;
}

int meri_big_call(meri_state *s, uint32_t id, uint64_t *a)
{
    jmp_buf jb;
    meri_big *volatile r = NULL;
    int res;

    switch (id) {
    case LIMBA_RT_BIG_FROM_I64:
    case LIMBA_RT_BIG_FROM_U64:
    case LIMBA_RT_BIG_LIT:
    case LIMBA_RT_BIG_TO_I64:
    case LIMBA_RT_BIG_TO_U64:
    case LIMBA_RT_BIG_TO_BITS:
    case LIMBA_RT_BIG_FROM_F64:
    case LIMBA_RT_BIG_TO_F64:
    case LIMBA_RT_BIG_TO_F32:
    case LIMBA_RT_BIG_ADD:
    case LIMBA_RT_BIG_SUB:
    case LIMBA_RT_BIG_MUL:
    case LIMBA_RT_BIG_NEG:
    case LIMBA_RT_BIG_ABS:
    case LIMBA_RT_BIG_DIV:
    case LIMBA_RT_BIG_REM:
    case LIMBA_RT_BIG_MOD:
    case LIMBA_RT_BIG_POW:
    case LIMBA_RT_BIG_CMP:
    case LIMBA_RT_BIG_SIGN:
    case LIMBA_RT_PRINT_BIG:
    case LIMBA_RT_STR_FROM_BIG:
    case LIMBA_RT_STR_TO_BIG:
        break;
    default:
        return -1;
    }
    bind(s);
    if (setjmp(jb)) { /* mini-gmp ran out of memory */
        cur_jmp = NULL;
        return trap(s, LIMBA_TRAP_NOMEM);
    }
    cur_jmp = &jb;
    res = 1;
    switch (id) {
    case LIMBA_RT_BIG_FROM_I64:
    case LIMBA_RT_BIG_FROM_U64:
    case LIMBA_RT_BIG_LIT:
    case LIMBA_RT_BIG_FROM_F64:
    case LIMBA_RT_BIG_ADD:
    case LIMBA_RT_BIG_SUB:
    case LIMBA_RT_BIG_MUL:
    case LIMBA_RT_BIG_NEG:
    case LIMBA_RT_BIG_ABS:
    case LIMBA_RT_BIG_DIV:
    case LIMBA_RT_BIG_REM:
    case LIMBA_RT_BIG_MOD:
    case LIMBA_RT_BIG_POW: {
        /* the operands that are refs: a[0] of the arithmetic, a[1] of
           the functions of two numbers */
        bool one = id != LIMBA_RT_BIG_FROM_I64 && id != LIMBA_RT_BIG_FROM_U64 &&
                   id != LIMBA_RT_BIG_LIT && id != LIMBA_RT_BIG_FROM_F64;
        bool two = id == LIMBA_RT_BIG_ADD || id == LIMBA_RT_BIG_SUB ||
                   id == LIMBA_RT_BIG_MUL || id == LIMBA_RT_BIG_DIV ||
                   id == LIMBA_RT_BIG_REM || id == LIMBA_RT_BIG_MOD;
        mpz_srcptr x = one ? num(a[0]) : NULL, y = two ? num(a[1]) : NULL;
        if (!(r = fresh(s))) {
            res = trap(s, LIMBA_TRAP_NOMEM);
            break;
        }
        switch (id) {
        case LIMBA_RT_BIG_FROM_I64:
            mpz_set_si(r->z, (long)(int64_t)a[0]);
            break;
        case LIMBA_RT_BIG_FROM_U64:
            mpz_set_ui(r->z, (unsigned long)a[0]);
            break;
        case LIMBA_RT_BIG_LIT: /* decimal digits, a '-' first */
            if (mpz_set_str(r->z, meri_str_of(a[0])->data, 10))
                res = trap(s, LIMBA_TRAP_CONVERSION);
            break;
        case LIMBA_RT_BIG_FROM_F64: {
            double d;
            memcpy(&d, &a[0], sizeof(d));
            if (!isfinite(d) || d != trunc(d))
                res = trap(s, LIMBA_TRAP_CONVERSION);
            else
                mpz_set_d(r->z, d); /* exact: an integer */
            break;
        }
        case LIMBA_RT_BIG_ADD:
            mpz_add(r->z, x, y);
            break;
        case LIMBA_RT_BIG_SUB:
            mpz_sub(r->z, x, y);
            break;
        case LIMBA_RT_BIG_MUL:
            mpz_mul(r->z, x, y);
            break;
        case LIMBA_RT_BIG_NEG:
            mpz_neg(r->z, x);
            break;
        case LIMBA_RT_BIG_ABS:
            mpz_abs(r->z, x);
            break;
        case LIMBA_RT_BIG_DIV:
        case LIMBA_RT_BIG_REM:
        case LIMBA_RT_BIG_MOD:
            if (!mpz_sgn(y)) {
                res = trap(s, LIMBA_TRAP_DIVZERO);
                break;
            }
            if (id == LIMBA_RT_BIG_DIV)
                mpz_tdiv_q(r->z, x, y); /* toward zero */
            else if (id == LIMBA_RT_BIG_REM)
                mpz_tdiv_r(r->z, x, y); /* the sign of the dividend */
            else
                mpz_fdiv_r(r->z, x, y); /* the sign of the divisor */
            break;
        case LIMBA_RT_BIG_POW:
            res = power(s, r, x, a[1]);
            break;
        }
        if (res == 1)
            give(a, r);
        break;
    }
    case LIMBA_RT_BIG_TO_I64: {
        mpz_srcptr x = num(a[0]);
        bool fits = mpz_fits_slong_p(x);
        if (fits)
            put64(a[1], (uint64_t)(int64_t)mpz_get_si(x));
        a[0] = fits;
        break;
    }
    case LIMBA_RT_BIG_TO_U64: {
        mpz_srcptr x = num(a[0]);
        bool fits = mpz_sgn(x) >= 0 && mpz_fits_ulong_p(x);
        if (fits)
            put64(a[1], (uint64_t)mpz_get_ui(x));
        a[0] = fits;
        break;
    }
    case LIMBA_RT_BIG_TO_BITS: { /* two's complement of the low 64 bits */
        mpz_srcptr x = num(a[0]);
        uint64_t low = mpz_getlimbn(x, 0);
        a[0] = mpz_sgn(x) < 0 ? 0 - low : low;
        break;
    }
    case LIMBA_RT_BIG_TO_F64:
        a[0] = fbits(to_f64(num(a[0])));
        break;
    case LIMBA_RT_BIG_TO_F32:
        a[0] = f32bits(to_f32(num(a[0])));
        break;
    case LIMBA_RT_BIG_CMP: { /* exactly -1, 0 or 1 */
        int c = mpz_cmp(num(a[0]), num(a[1]));
        a[0] = (uint64_t)(int64_t)(c < 0 ? -1 : c > 0);
        break;
    }
    case LIMBA_RT_BIG_SIGN:
        a[0] = (uint64_t)(int64_t)mpz_sgn(num(a[0]));
        break;
    case LIMBA_RT_PRINT_BIG: {
        char *t = text(num(a[0]));
        if (!t) {
            res = trap(s, LIMBA_TRAP_NOMEM);
            break;
        }
        fputs(t, s->env->out);
        free(t);
        break;
    }
    case LIMBA_RT_STR_FROM_BIG: {
        char *t = text(num(a[0]));
        meri_str *x = t ? meri_state_str(s, t, strlen(t)) : NULL;
        free(t);
        if (!x) {
            res = trap(s, LIMBA_TRAP_NOMEM);
            break;
        }
        a[0] = meri_str_value(x);
        break;
    }
    case LIMBA_RT_STR_TO_BIG: { /* val: a store of the number read */
        const meri_str *t = meri_str_of(a[0]);
        char *digits = malloc(t->len + 1);
        size_t n;
        unsigned base;
        bool neg;
        uint64_t old;
        if (!digits) {
            res = trap(s, LIMBA_TRAP_NOMEM);
            break;
        }
        if (!limba_val_big(t->data, t->len, digits, &n, &base, &neg)) {
            free(digits);
            a[0] = 0;
            break;
        }
        digits[n] = 0;
        if (!(r = fresh(s))) {
            free(digits);
            res = trap(s, LIMBA_TRAP_NOMEM);
            break;
        }
        if (mpz_set_str(r->z, digits, (int)base)) {
            free(digits);
            res = trap(s, LIMBA_TRAP_CONVERSION);
            break;
        }
        free(digits);
        if (neg)
            mpz_neg(r->z, r->z);
        memcpy(&old, (const void *)(uintptr_t)MERI_ADDR(a[1]), sizeof(old));
        put64(a[1], (uint64_t)(uintptr_t)r);
        meri_big_release(s, old);
        a[0] = 1;
        break;
    }
    }
    cur_jmp = NULL;
    return res;
}
