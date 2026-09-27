/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * rt.c - the functions of runtime.def. Each takes its arguments in a[0..n)
 * as the IR gives them (canonical integers, the bits of a float, a str
 * value) and leaves its result in a[0]. The reals are written, and the
 * numbers of val read, by the functions of Limba (limba/fmt.h,
 * limba/val.h), never by a copy of them.
 */
#include "vm/rt.h"

#include "limba/fmt.h"
#include "limba/val.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool meri_rt_trap(meri_state *s, int64_t code)
{
    s->status = MERI_TRAP;
    s->code = code;
    return false;
}

bool meri_state_take(meri_state *s, uint64_t n)
{
    if (n > s->budget || s->used > s->budget - n)
        return false;
    s->used += n;
    return true;
}

void meri_state_give(meri_state *s, uint64_t n)
{
    s->used -= n < s->used ? n : s->used;
}

/* the bytes a string of n takes from the budget */
static uint64_t str_bytes(size_t n)
{
    return (uint64_t)sizeof(meri_str) + n + 1;
}

meri_str *meri_state_alloc(meri_state *s, size_t n)
{
    meri_str *x;

    if (s->nstrs == s->capstrs) {
        size_t cap = s->capstrs ? 2 * s->capstrs : 256;
        meri_str **strs = cap > SIZE_MAX / sizeof(*strs)
                              ? NULL
                              : realloc(s->strs, cap * sizeof(*strs));
        if (!strs)
            return NULL;
        s->strs = strs;
        s->capstrs = cap;
    }
    if (n > SIZE_MAX - sizeof(meri_str) - 1 ||
        !meri_state_take(s, str_bytes(n)))
        return NULL;
    x = meri_str_alloc(n);
    if (!x) {
        meri_state_give(s, str_bytes(n));
        return NULL;
    }
    x->rc = 1;
    x->slot = s->nstrs;
    s->strs[s->nstrs++] = x;
    return x;
}

meri_str *meri_state_str(meri_state *s, const char *p, size_t n)
{
    meri_str *x = meri_state_alloc(s, n);

    if (x && n)
        memcpy(x->data, p, n);
    return x;
}

meri_str *meri_state_immortal(meri_state *s, const char *p, size_t n)
{
    meri_str *x = meri_state_str(s, p, n);

    if (x)
        x->rc = MERI_RC_IMMORTAL;
    return x;
}

void meri_state_release(meri_state *s, uint64_t v)
{
    meri_str *x = (meri_str *)(uintptr_t)v, *last;

    if (!x || x->rc == MERI_RC_IMMORTAL || --x->rc)
        return;
    last = s->strs[--s->nstrs];
    s->strs[x->slot] = last;
    last->slot = x->slot;
    meri_state_give(s, str_bytes(x->len));
    free(x);
}

/* retain or release the str of one value of type t at p */
static void rc_walk(meri_state *s, uintptr_t p, limba_id t, int d)
{
    const limba_module *m = s->p->m;
    const limba_type *ty = &m->types[t];
    uint32_t i;

    if (!s->p->holds_str[t])
        return;
    switch (ty->kind) {
    case LIMBA_TK_STR: {
        uint64_t v;
        memcpy(&v, (const void *)p, sizeof(v));
        if (d > 0)
            meri_str_retain(v);
        else
            meri_state_release(s, v);
        return;
    }
    case LIMBA_TK_ARRAY:
        for (i = 0; i < ty->count; i++)
            rc_walk(s, p + (uintptr_t)i * m->types[ty->elem].size, ty->elem, d);
        return;
    case LIMBA_TK_STRUCT:
        for (i = 0; i < ty->count; i++) {
            const limba_member *f = &m->members[ty->first + i];
            rc_walk(s, p + f->offset, f->type, d);
        }
        return;
    }
}

void meri_state_rc(meri_state *s, uint64_t p, limba_id t, uint64_t n, int d)
{
    uint64_t size = s->p->m->types[t].size, i;

    for (i = 0; i < n; i++)
        rc_walk(s, (uintptr_t)(p + i * size), t, d);
}

void meri_state_free_strs(meri_state *s)
{
    size_t i;

    for (i = 0; i < s->nstrs; i++)
        free(s->strs[i]);
    free(s->strs);
    s->strs = NULL;
    s->nstrs = s->capstrs = 0;
}

static double dv(uint64_t v)
{
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static uint64_t dbits(double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

static float fv(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void out(meri_state *s, const void *p, size_t n)
{
    if (n)
        fwrite(p, 1, n, s->env->out);
}

/* a new string as the result, or NOMEM */
static bool ret_str(meri_state *s, uint64_t *a, const char *p, size_t n)
{
    meri_str *x = meri_state_str(s, p, n);

    if (!x)
        return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
    a[0] = meri_str_value(x);
    return true;
}

/* UTF-8 of a code point into buf (4 bytes); its length */
static size_t utf8(uint32_t c, char *buf)
{
    size_t n, k;

    if (c < 0x80) {
        buf[0] = (char)c;
        return 1;
    }
    n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    for (k = n; k-- > 1;) {
        buf[k] = (char)(0x80 | (c & 0x3f));
        c >>= 6;
    }
    buf[0] = (char)((0xf00 >> n) | c);
    return n;
}

/* a line of in without its LF or CR LF; *ok false at the end of the file,
   with an empty line. NULL when memory is exhausted */
static meri_str *line(meri_state *st, FILE *in, bool *ok)
{
    char *buf = NULL;
    size_t cap = 0, len;
    ssize_t got = in ? getline(&buf, &cap, in) : -1;
    meri_str *s;

    *ok = got >= 0;
    len = *ok ? (size_t)got : 0;
    if (len && buf[len - 1] == '\n')
        len--;
    if (len && buf[len - 1] == '\r')
        len--;
    s = meri_state_str(st, buf, len);
    free(buf);
    return s;
}

/* store the bits of type t at p, as the IR's store */
static void put(uint64_t p, limba_id t, uint64_t v)
{
    void *q = (void *)(uintptr_t)p;

    if (t == LIMBA_T_F32) {
        uint32_t x = (uint32_t)v;
        memcpy(q, &x, 4);
    } else {
        memcpy(q, &v, 8);
    }
}

/* the powers of runtime.def: int_pow traps on a signed overflow,
   uint_pow on an unsigned one, bits_pow wraps modulo 2^64 */
static bool power(meri_state *s, uint32_t id, uint64_t *a)
{
    uint64_t ex = a[1]; /* without a sign */

    if (id == LIMBA_RT_INT_POW) {
        int64_t base = (int64_t)a[0], acc = 1;
        while (ex) {
            if ((ex & 1) && __builtin_mul_overflow(acc, base, &acc))
                return meri_rt_trap(s, LIMBA_TRAP_OVERFLOW);
            ex >>= 1;
            if (ex && __builtin_mul_overflow(base, base, &base))
                return meri_rt_trap(s, LIMBA_TRAP_OVERFLOW);
        }
        a[0] = (uint64_t)acc;
    } else {
        uint64_t base = a[0], acc = 1;
        bool wrap = id == LIMBA_RT_BITS_POW;
        while (ex) {
            if ((ex & 1) && __builtin_mul_overflow(acc, base, &acc) && !wrap)
                return meri_rt_trap(s, LIMBA_TRAP_OVERFLOW);
            ex >>= 1;
            if (ex && __builtin_mul_overflow(base, base, &base) && !wrap)
                return meri_rt_trap(s, LIMBA_TRAP_OVERFLOW);
        }
        a[0] = acc;
    }
    return true;
}

/* str_to_i64 and str_to_u64: (s, p, lo, hi) -> i1, the value at p */
static bool to_int(uint32_t id, uint64_t *a)
{
    uint64_t mag;
    const meri_str *x = meri_str_of(a[0]);
    bool neg, ok = limba_val_int(x->data, x->len, &mag, &neg);
    int64_t v = (int64_t)(neg ? 0 - mag : mag);

    if (id == LIMBA_RT_STR_TO_I64)
        ok = ok && mag <= (uint64_t)INT64_MAX + neg && v >= (int64_t)a[2] &&
             v <= (int64_t)a[3];
    else
        ok = ok && (!neg || mag == 0) && mag >= a[2] && mag <= a[3];
    if (ok)
        put(a[1], LIMBA_T_I64, neg ? 0 - mag : mag);
    a[0] = ok;
    return true;
}

/* the families below answer DONE, STOP (the run stops: s->status says
   why) or NOT_MINE */
enum { STOP, DONE, NOT_MINE };

static int nomem(meri_state *s)
{
    meri_rt_trap(s, LIMBA_TRAP_NOMEM);
    return STOP;
}

static int console(meri_state *s, uint32_t id, uint64_t *a)
{
    char buf[LIMBA_FMT_F64_MAX];
    int n;

    switch (id) {
    case LIMBA_RT_PRINT_I64:
        n = snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)a[0]);
        out(s, buf, (size_t)n);
        return DONE;
    case LIMBA_RT_PRINT_U64:
        n = snprintf(buf, sizeof(buf), "%" PRIu64, a[0]);
        out(s, buf, (size_t)n);
        return DONE;
    case LIMBA_RT_PRINT_F64:
        out(s, buf, limba_fmt_f64(buf, dv(a[0])));
        return DONE;
    case LIMBA_RT_PRINT_F32:
        out(s, buf, limba_fmt_f32(buf, fv(a[0])));
        return DONE;
    case LIMBA_RT_PRINT_STR: {
        const meri_str *x = meri_str_of(a[0]);
        out(s, x->data, x->len);
        return DONE;
    }
    case LIMBA_RT_PRINT_NL:
        out(s, "\n", 1);
        return DONE;
    case LIMBA_RT_PRINT_BOOL:
        out(s, a[0] & 1 ? "true" : "false", a[0] & 1 ? 4 : 5);
        return DONE;
    case LIMBA_RT_PRINT_CHAR:
        out(s, buf, utf8((uint32_t)a[0], buf));
        return DONE;
    case LIMBA_RT_PRINT_BYTE:
        buf[0] = (char)a[0];
        out(s, buf, 1);
        return DONE;
    case LIMBA_RT_PRINT_STR_W: { /* right-aligned, in code points */
        const meri_str *x = meri_str_of(a[0]);
        int64_t width = (int32_t)a[1], chars = 0;
        size_t i;
        for (i = 0; i < x->len; i++)
            chars += ((unsigned char)x->data[i] & 0xc0) != 0x80;
        for (; width > chars; width--)
            out(s, " ", 1);
        out(s, x->data, x->len);
        return DONE;
    }
    case LIMBA_RT_READ_LINE: { /* (p) -> i1: the line at p, "" at the end */
        bool ok;
        meri_str *x = line(s, s->env->in, &ok);
        if (!x)
            return nomem(s);
        {
            /* as store str: the line's reference goes to memory, the old
               value is released */
            uint64_t old;
            memcpy(&old, (const void *)(uintptr_t)a[0], sizeof(old));
            put(a[0], LIMBA_T_STR, meri_str_value(x));
            meri_state_release(s, old);
        }
        a[0] = ok;
        return DONE;
    }
    }
    return NOT_MINE;
}

static int strings(meri_state *s, uint32_t id, uint64_t *a)
{
    char buf[LIMBA_FMT_FIXED_MAX];
    int n;

    switch (id) {
    case LIMBA_RT_STR_CONCAT: {
        const meri_str *x = meri_str_of(a[0]), *y = meri_str_of(a[1]);
        meri_str *r;
        if (x->len > SIZE_MAX / 2 || y->len > SIZE_MAX / 2 ||
            !(r = meri_state_alloc(s, x->len + y->len)))
            return nomem(s);
        memcpy(r->data, x->data, x->len);
        memcpy(r->data + x->len, y->data, y->len);
        a[0] = meri_str_value(r);
        return DONE;
    }
    case LIMBA_RT_STR_LEN:
        a[0] = meri_str_of(a[0])->len;
        return DONE;
    case LIMBA_RT_STR_CMP: { /* -1, 0 or 1, an i32 */
        const meri_str *x = meri_str_of(a[0]), *y = meri_str_of(a[1]);
        size_t k = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->data, y->data, k);
        if (!c)
            c = x->len < y->len ? -1 : x->len > y->len;
        a[0] = (uint64_t)(int64_t)(c < 0 ? -1 : c > 0);
        return DONE;
    }
    case LIMBA_RT_STR_MID: { /* (s, start from 0, length), clamped */
        const meri_str *x = meri_str_of(a[0]);
        int64_t start = (int64_t)a[1], len = (int64_t)a[2];
        if (start < 0)
            start = 0;
        if ((uint64_t)start > x->len)
            start = (int64_t)x->len;
        if (len < 0 || (uint64_t)len > x->len - (uint64_t)start)
            len = (int64_t)(x->len - (uint64_t)start);
        return ret_str(s, a, x->data + start, (size_t)len) ? DONE : STOP;
    }
    case LIMBA_RT_STR_PTR:
        a[0] = (uint64_t)(uintptr_t)meri_str_of(a[0])->data;
        return DONE;
    case LIMBA_RT_STR_FROM_I64:
        n = snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)a[0]);
        return ret_str(s, a, buf, (size_t)n) ? DONE : STOP;
    case LIMBA_RT_STR_FROM_U64:
        n = snprintf(buf, sizeof(buf), "%" PRIu64, a[0]);
        return ret_str(s, a, buf, (size_t)n) ? DONE : STOP;
    case LIMBA_RT_STR_FROM_F64:
        return ret_str(s, a, buf, limba_fmt_f64(buf, dv(a[0]))) ? DONE : STOP;
    case LIMBA_RT_STR_FROM_F32:
        return ret_str(s, a, buf, limba_fmt_f32(buf, fv(a[0]))) ? DONE : STOP;
    case LIMBA_RT_STR_FROM_F64_FIXED:
        return ret_str(s, a, buf,
                       limba_fmt_f64_fixed(buf, dv(a[0]), (int32_t)a[1]))
                   ? DONE
                   : STOP;
    case LIMBA_RT_STR_FROM_CHAR:
        return ret_str(s, a, buf, utf8((uint32_t)a[0], buf)) ? DONE : STOP;
    case LIMBA_RT_STR_FROM_BOOL:
        return ret_str(s, a, a[0] & 1 ? "true" : "false", a[0] & 1 ? 4 : 5)
                   ? DONE
                   : STOP;
    case LIMBA_RT_STR_TO_I64:
    case LIMBA_RT_STR_TO_U64:
        return to_int(id, a) ? DONE : STOP;
    case LIMBA_RT_STR_TO_F64:
    case LIMBA_RT_STR_TO_F32: {
        bool f32 = id == LIMBA_RT_STR_TO_F32;
        uint64_t v;
        const meri_str *x = meri_str_of(a[0]);
        bool ok = limba_val_real(x->data, x->len, f32, &v);
        if (ok)
            put(a[1], f32 ? LIMBA_T_F32 : LIMBA_T_F64, v);
        a[0] = ok;
        return DONE;
    }
    }
    return NOT_MINE;
}

static bool maths(uint32_t id, uint64_t *a)
{
    double x = dv(a[0]), r;

    switch (id) {
    case LIMBA_RT_MATH_SQRT:
        r = sqrt(x);
        break;
    case LIMBA_RT_MATH_SIN:
        r = sin(x);
        break;
    case LIMBA_RT_MATH_COS:
        r = cos(x);
        break;
    case LIMBA_RT_MATH_TAN:
        r = tan(x);
        break;
    case LIMBA_RT_MATH_ATAN:
        r = atan(x);
        break;
    case LIMBA_RT_MATH_EXP:
        r = exp(x);
        break;
    case LIMBA_RT_MATH_LN:
        r = log(x);
        break;
    case LIMBA_RT_MATH_TRUNC:
        r = trunc(x);
        break;
    case LIMBA_RT_MATH_FLOOR:
        r = floor(x);
        break;
    case LIMBA_RT_MATH_CEIL:
        r = ceil(x);
        break;
    case LIMBA_RT_MATH_POW:
        r = pow(x, dv(a[1]));
        break;
    default:
        return false;
    }
    a[0] = dbits(r);
    return true;
}

bool meri_rt_call(meri_state *s, uint32_t id, uint64_t *a)
{
    int r;

    switch (id) {
    case LIMBA_RT_MEM_ALLOC: {
        /* a freed block is never given back in the first cut: it stays
           counted */
        uint64_t n = a[0] ? a[0] : 1;
        if ((int64_t)a[0] < 0 || !meri_state_take(s, n))
            return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
        a[0] = meri_heap_alloc(&s->heap, a[0]);
        if (!a[0]) {
            meri_state_give(s, n);
            return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
        }
        return true;
    }
    case LIMBA_RT_MEM_FREE:
        return meri_heap_free(&s->heap, a[0]) ||
               meri_rt_trap(s, LIMBA_TRAP_INVALID_FREE);
    case LIMBA_RT_PTR_LIVE:
        a[0] = meri_heap_live(&s->heap, a[0]);
        return true;
    case LIMBA_RT_ARG_COUNT:
        a[0] = (uint64_t)(s->env->argc > 0 ? s->env->argc : 0);
        return true;
    case LIMBA_RT_ARG: { /* from 1; "" outside */
        int32_t i = (int32_t)a[0];
        const char *x = i >= 1 && i <= s->env->argc ? s->env->argv[i - 1] : "";
        return ret_str(s, a, x, strlen(x));
    }
    case LIMBA_RT_HALT:
        s->status = MERI_HALT;
        s->code = (int32_t)a[0];
        return false;
    case LIMBA_RT_INT_POW:
    case LIMBA_RT_UINT_POW:
    case LIMBA_RT_BITS_POW:
        return power(s, id, a);
    }
    if (maths(id, a))
        return true;
    if ((r = console(s, id, a)) == NOT_MINE)
        r = strings(s, id, a);
    if (r == NOT_MINE) { /* a table newer than this runtime */
        s->status = MERI_UNSUPPORTED;
        return false;
    }
    return r == DONE;
}
