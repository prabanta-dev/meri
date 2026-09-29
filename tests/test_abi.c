/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_abi.c - the probes of the calling convention (src/vm/abi.c). Every
 * probe is a C function with external linkage, so the compiler lays out
 * its calls by the ABI: it is the oracle. Each is called directly and
 * through meri_abi_call, with the same arguments, and the results are
 * compared field by field (the padding of a struct in registers is not
 * defined). A probe mixes every argument into its result, weighted by its
 * place, so that a value in the wrong register, or two swapped, shows.
 */
#include "vm/abi.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures, cases;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_abi: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* the scalar regions of a value of type t at p and q are equal */
static bool same(const meri_abi_type *t, const void *p, const void *q)
{
    uint32_t i;

    if (t->kind != MERI_ABI_STRUCT)
        return !memcmp(p, q, t->size);
    for (i = 0; i < t->nfields; i++)
        if (!same(t->fields[i], (const char *)p + t->offsets[i],
                  (const char *)q + t->offsets[i]))
            return false;
    return true;
}

/* one call through the ABI, its result compared with the direct one */
static void probe(const char *name, void *fn, const meri_abi_type *rt,
                  uint32_t n, const meri_abi_type *const *at,
                  const void *const *av, const void *want)
{
    unsigned char got[64];
    const char *why = "";

    cases++;
    memset(got, 0xa5, sizeof(got));
    if (!meri_abi_call(fn, rt, n, at, av, got, &why)) {
        CHECK(0, "%s: refused: %s", name, why);
        return;
    }
    CHECK(rt->kind == MERI_ABI_VOID || same(rt, got, want),
          "%s: a result other than the direct call's", name);
}

/* ---- the types of the structs: fields, offsets from the compiler */

#define T(x) (&meri_abi_##x)
#define STRUCT_TYPE(var, S, ...)                                               \
    static const meri_abi_type *const var##_f[] = {__VA_ARGS__};               \
    static const meri_abi_type var = {                                         \
        MERI_ABI_STRUCT, sizeof(S),                                            \
        _Alignof(S),     sizeof(var##_f) / sizeof(var##_f[0]),                 \
        var##_f,         var##_o}

typedef struct {
    int8_t a, b;
} S1; /* 2 bytes, INTEGER */
static const uint32_t ts1_o[] = {offsetof(S1, a), offsetof(S1, b)};
STRUCT_TYPE(ts1, S1, T(s8), T(s8));

typedef struct {
    int32_t a;
    float b;
} S2; /* 8 bytes, an int and a float in one eightbyte: INTEGER */
static const uint32_t ts2_o[] = {offsetof(S2, a), offsetof(S2, b)};
STRUCT_TYPE(ts2, S2, T(s32), T(f32));

typedef struct {
    float a, b;
} S3; /* SSE */
static const uint32_t ts3_o[] = {offsetof(S3, a), offsetof(S3, b)};
STRUCT_TYPE(ts3, S3, T(f32), T(f32));

typedef struct {
    double a, b;
} S4; /* SSE, SSE */
static const uint32_t ts4_o[] = {offsetof(S4, a), offsetof(S4, b)};
STRUCT_TYPE(ts4, S4, T(f64), T(f64));

typedef struct {
    int64_t a;
    double b;
} S5; /* INTEGER, SSE */
static const uint32_t ts5_o[] = {offsetof(S5, a), offsetof(S5, b)};
STRUCT_TYPE(ts5, S5, T(s64), T(f64));

typedef struct {
    double a;
    int32_t b, c;
} S6; /* SSE, INTEGER */
static const uint32_t ts6_o[] = {offsetof(S6, a), offsetof(S6, b),
                                 offsetof(S6, c)};
STRUCT_TYPE(ts6, S6, T(f64), T(s32), T(s32));

typedef struct {
    float a, b, c;
} S7; /* 12 bytes: SSE, SSE */
static const uint32_t ts7_o[] = {offsetof(S7, a), offsetof(S7, b),
                                 offsetof(S7, c)};
STRUCT_TYPE(ts7, S7, T(f32), T(f32), T(f32));

typedef struct {
    int64_t a, b, c;
} S8; /* 24 bytes: MEMORY */
static const uint32_t ts8_o[] = {offsetof(S8, a), offsetof(S8, b),
                                 offsetof(S8, c)};
STRUCT_TYPE(ts8, S8, T(s64), T(s64), T(s64));

typedef struct {
    uint8_t a;
    int16_t b;
    int32_t c;
} S9; /* 8 bytes with a hole: INTEGER */
static const uint32_t ts9_o[] = {offsetof(S9, a), offsetof(S9, b),
                                 offsetof(S9, c)};
STRUCT_TYPE(ts9, S9, T(u8), T(s16), T(s32));

typedef struct {
    S3 in;
    double d;
} S10; /* nested: SSE, SSE */
static const uint32_t ts10_o[] = {offsetof(S10, in), offsetof(S10, d)};
STRUCT_TYPE(ts10, S10, &ts3, T(f64));

typedef struct {
    uint8_t a, b, c;
} S11; /* 3 bytes: INTEGER */
static const uint32_t ts11_o[] = {offsetof(S11, a), offsetof(S11, b),
                                  offsetof(S11, c)};
STRUCT_TYPE(ts11, S11, T(u8), T(u8), T(u8));

typedef struct {
    int64_t a, b;
} S12; /* INTEGER, INTEGER */
static const uint32_t ts12_o[] = {offsetof(S12, a), offsetof(S12, b)};
STRUCT_TYPE(ts12, S12, T(s64), T(s64));

typedef struct {
    double a;
    float b;
    int8_t c;
} S13; /* SSE, then a float and an int in one eightbyte: INTEGER */
static const uint32_t ts13_o[] = {offsetof(S13, a), offsetof(S13, b),
                                  offsetof(S13, c)};
STRUCT_TYPE(ts13, S13, T(f64), T(f32), T(s8));

/* ---- the probes (external: the ABI of the compiler) */

int64_t p_ints(int8_t a, uint8_t b, int16_t c, uint16_t d, int32_t e,
               uint32_t f, int64_t g, uint64_t h);
int64_t p_ints(int8_t a, uint8_t b, int16_t c, uint16_t d, int32_t e,
               uint32_t f, int64_t g, uint64_t h)
{
    /* modulo 2^64: no overflow */
    return (int64_t)((uint64_t)a + 3u * b + 5u * (uint64_t)c + 7u * d +
                     11u * (uint64_t)e + 13u * (uint64_t)f + 17u * (uint64_t)g +
                     19u * h);
}

int8_t p_ret_s8(int32_t x);
int8_t p_ret_s8(int32_t x)
{
    return (int8_t)(x * 3);
}
uint16_t p_ret_u16(int32_t x);
uint16_t p_ret_u16(int32_t x)
{
    return (uint16_t)(x * 5);
}
float p_ret_f32(float x, double y);
float p_ret_f32(float x, double y)
{
    return x * 2.0f + (float)y;
}

double p_floats(float a, double b, float c, double d);
double p_floats(float a, double b, float c, double d)
{
    return a + 3 * b + 5 * c + 7 * d;
}

/* 10 integers and 10 reals, interleaved: 4 and 2 of them on the stack */
double p_many(int64_t i0, double d0, int64_t i1, double d1, int64_t i2,
              double d2, int64_t i3, double d3, int64_t i4, double d4,
              int64_t i5, double d5, int64_t i6, double d6, int64_t i7,
              double d7, int64_t i8, double d8, int64_t i9, double d9);
double p_many(int64_t i0, double d0, int64_t i1, double d1, int64_t i2,
              double d2, int64_t i3, double d3, int64_t i4, double d4,
              int64_t i5, double d5, int64_t i6, double d6, int64_t i7,
              double d7, int64_t i8, double d8, int64_t i9, double d9)
{
    return (double)(i0 + 2 * i1 + 3 * i2 + 4 * i3 + 5 * i4 + 6 * i5 + 7 * i6 +
                    8 * i7 + 9 * i8 + 10 * i9) +
           d0 + 2 * d1 + 3 * d2 + 4 * d3 + 5 * d4 + 6 * d5 + 7 * d6 + 8 * d7 +
           9 * d8 + 10 * d9;
}

/* a struct passed and returned, changed in every field */
S1 p_s1(S1 x, int32_t k);
S1 p_s1(S1 x, int32_t k)
{
    return (S1){(int8_t)(x.a + k), (int8_t)(x.b - k)};
}
S2 p_s2(S2 x, int32_t k);
S2 p_s2(S2 x, int32_t k)
{
    return (S2){x.a * k, x.b + (float)k};
}
S3 p_s3(S3 x, int32_t k);
S3 p_s3(S3 x, int32_t k)
{
    return (S3){x.a + (float)k, x.b * (float)k};
}
S4 p_s4(S4 x, int32_t k);
S4 p_s4(S4 x, int32_t k)
{
    return (S4){x.a - k, x.b * k};
}
S5 p_s5(S5 x, int32_t k);
S5 p_s5(S5 x, int32_t k)
{
    return (S5){x.a * k, x.b + k};
}
S6 p_s6(S6 x, int32_t k);
S6 p_s6(S6 x, int32_t k)
{
    return (S6){x.a * k, x.b + k, x.c - k};
}
S7 p_s7(S7 x, int32_t k);
S7 p_s7(S7 x, int32_t k)
{
    return (S7){x.a + (float)k, x.b - (float)k, x.c * (float)k};
}
S8 p_s8(S8 x, int32_t k);
S8 p_s8(S8 x, int32_t k)
{
    return (S8){x.a + k, x.b - k, x.c * k};
}
S9 p_s9(S9 x, int32_t k);
S9 p_s9(S9 x, int32_t k)
{
    return (S9){(uint8_t)(x.a + k), (int16_t)(x.b - k), x.c * k};
}
S10 p_s10(S10 x, int32_t k);
S10 p_s10(S10 x, int32_t k)
{
    return (S10){{x.in.a * (float)k, x.in.b - (float)k}, x.d + k};
}
S11 p_s11(S11 x, int32_t k);
S11 p_s11(S11 x, int32_t k)
{
    return (S11){(uint8_t)(x.a + k), (uint8_t)(x.b + 2 * k),
                 (uint8_t)(x.c + 3 * k)};
}
S13 p_s13(S13 x, int32_t k);
S13 p_s13(S13 x, int32_t k)
{
    return (S13){x.a * k, x.b + (float)k, (int8_t)(x.c - k)};
}

/* five integers, then a struct of two INTEGER eightbytes with one
   register left: the struct goes whole to the stack, and the integer
   after it takes the register */
int64_t p_gpr_full(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, S12 s,
                   int64_t z);
int64_t p_gpr_full(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, S12 s,
                   int64_t z)
{
    return (int64_t)((uint64_t)a + 2u * (uint64_t)b + 3u * (uint64_t)c +
                     4u * (uint64_t)d + 5u * (uint64_t)e + 6u * (uint64_t)s.a +
                     7u * (uint64_t)s.b + 8u * (uint64_t)z);
}

/* seven reals, then a struct of two SSE eightbytes with one register
   left: to the stack; the real after it takes xmm7 */
double p_sse_full(double a, double b, double c, double d, double e, double f,
                  double g, S4 s, double z);
double p_sse_full(double a, double b, double c, double d, double e, double f,
                  double g, S4 s, double z)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * s.a +
           9 * s.b + 10 * z;
}

/* a result in memory: the address of the result takes rdi */
S8 p_mem_ret(int64_t a, S8 in, double d, int64_t b);
S8 p_mem_ret(int64_t a, S8 in, double d, int64_t b)
{
    return (S8){a + in.a, in.b - b, in.c * (int64_t)d};
}

/* the stack at the call is 16-byte aligned, with one word of arguments on
   it (seven integers) and with two (eight): a local aligned to 16 shows
   how the stack came */
int64_t p_align(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                int64_t f, int64_t g);
int64_t p_align(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                int64_t f, int64_t g)
{
    _Alignas(16) volatile char probe[16];
    probe[0] = (char)(a + b + c + d + e + f + g);
    return (int64_t)((uintptr_t)probe & 15) + probe[0] * 16;
}
int64_t p_align2(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                 int64_t f, int64_t g, int64_t h);
int64_t p_align2(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                 int64_t f, int64_t g, int64_t h)
{
    _Alignas(16) volatile char probe[16];
    probe[0] = (char)(a + b + c + d + e + f + g + h);
    return (int64_t)((uintptr_t)probe & 15) + probe[0] * 16;
}

/* a harmless callee for the calls that must be refused */
void p_none(void);
void p_none(void)
{
    failures++;
}

/* a pointer: the callee writes through it */
void p_ptr(int64_t *p, int32_t k);
void p_ptr(int64_t *p, int32_t k)
{
    p[0] = p[0] * k + 1;
}

/* ---- the cases */

#define ARGS(...)                                                              \
    (const meri_abi_type *const[])                                             \
    {                                                                          \
        __VA_ARGS__                                                            \
    }
#define VALS(...)                                                              \
    (const void *const[])                                                      \
    {                                                                          \
        __VA_ARGS__                                                            \
    }

static void scalars(void)
{
    int8_t a = -128;
    uint8_t b = 255;
    int16_t c = -32768;
    uint16_t d = 65535;
    int32_t e = INT32_MIN, x = -77;
    uint32_t f = UINT32_MAX;
    int64_t g = INT64_MIN + 12345;
    uint64_t h = UINT64_MAX - 9;
    float fa = -1.5f;
    double db = 2.25, dd = -1e300;
    float fc = 3.0f;
    int64_t want = p_ints(a, b, c, d, e, f, g, h);
    int8_t w8 = p_ret_s8(x);
    uint16_t w16 = p_ret_u16(x);
    float wf = p_ret_f32(fa, db);
    double wd = p_floats(fa, db, fc, dd);

    probe("ints", (void *)p_ints, T(s64), 8,
          ARGS(T(s8), T(u8), T(s16), T(u16), T(s32), T(u32), T(s64), T(u64)),
          VALS(&a, &b, &c, &d, &e, &f, &g, &h), &want);
    probe("ret s8", (void *)p_ret_s8, T(s8), 1, ARGS(T(s32)), VALS(&x), &w8);
    probe("ret u16", (void *)p_ret_u16, T(u16), 1, ARGS(T(s32)), VALS(&x),
          &w16);
    probe("ret f32", (void *)p_ret_f32, T(f32), 2, ARGS(T(f32), T(f64)),
          VALS(&fa, &db), &wf);
    probe("floats", (void *)p_floats, T(f64), 4,
          ARGS(T(f32), T(f64), T(f32), T(f64)), VALS(&fa, &db, &fc, &dd), &wd);
}

static void many(void)
{
    int64_t i[10];
    double d[10], want;
    const meri_abi_type *at[20];
    const void *av[20];
    int k;

    for (k = 0; k < 10; k++) {
        i[k] = (k + 1) * 1000003LL * (k % 2 ? -1 : 1);
        d[k] = (k + 1) * 0.3125 * (k % 3 ? 1 : -1);
        at[2 * k] = T(s64), av[2 * k] = &i[k];
        at[2 * k + 1] = T(f64), av[2 * k + 1] = &d[k];
    }
    want = p_many(i[0], d[0], i[1], d[1], i[2], d[2], i[3], d[3], i[4], d[4],
                  i[5], d[5], i[6], d[6], i[7], d[7], i[8], d[8], i[9], d[9]);
    probe("many", (void *)p_many, T(f64), 20, at, av, &want);
}

#define STRUCT_CASE(n, S, ty, fn, ...)                                         \
    do {                                                                       \
        S x = __VA_ARGS__, w;                                                  \
        int32_t k = 7;                                                         \
        memset(&w, 0, sizeof(w));                                              \
        w = fn(x, k);                                                          \
        probe(n, (void *)fn, &ty, 2, ARGS(&ty, T(s32)), VALS(&x, &k), &w);     \
    } while (0)

static void structs(void)
{
    STRUCT_CASE("s1", S1, ts1, p_s1, {-100, 99});
    STRUCT_CASE("s2", S2, ts2, p_s2, {-123456, 1.25f});
    STRUCT_CASE("s3", S3, ts3, p_s3, {1.5f, -2.5f});
    STRUCT_CASE("s4", S4, ts4, p_s4, {1e10, -3.75});
    STRUCT_CASE("s5", S5, ts5, p_s5, {-987654321012LL, 0.125});
    STRUCT_CASE("s6", S6, ts6, p_s6, {-6.5, 2000000000, -3});
    STRUCT_CASE("s7", S7, ts7, p_s7, {1.0f, 2.0f, -3.5f});
    STRUCT_CASE("s8", S8, ts8, p_s8, {INT64_MIN / 3, 42, -7});
    STRUCT_CASE("s9", S9, ts9, p_s9, {250, -30000, 123456});
    STRUCT_CASE("s10", S10, ts10, p_s10, {{0.5f, 9.0f}, -1.0});
    STRUCT_CASE("s11", S11, ts11, p_s11, {1, 2, 250});
    STRUCT_CASE("s13", S13, ts13, p_s13, {2.5, -0.75f, -100});
}

static void exhaustion(void)
{
    int64_t a = 1, b = -2, c = 3, d = -4, e = 5, z = 99, wi;
    double f[7] = {1.5, -2.5, 3.5, -4.5, 5.5, -6.5, 7.5}, zd = -0.25, wd;
    S12 s = {1000, -2000};
    S4 s4 = {0.0625, -128.0};

    wi = p_gpr_full(a, b, c, d, e, s, z);
    probe("gpr full", (void *)p_gpr_full, T(s64), 7,
          ARGS(T(s64), T(s64), T(s64), T(s64), T(s64), &ts12, T(s64)),
          VALS(&a, &b, &c, &d, &e, &s, &z), &wi);
    wd = p_sse_full(f[0], f[1], f[2], f[3], f[4], f[5], f[6], s4, zd);
    probe("sse full", (void *)p_sse_full, T(f64), 9,
          ARGS(T(f64), T(f64), T(f64), T(f64), T(f64), T(f64), T(f64), &ts4,
               T(f64)),
          VALS(&f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &s4, &zd), &wd);
}

static void alignment(void)
{
    int64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7;
    int64_t w = p_align(a, b, c, d, e, f, g);

    CHECK(w % 16 == 0, "the direct call itself is not aligned");
    probe("align", (void *)p_align, T(s64), 7,
          ARGS(T(s64), T(s64), T(s64), T(s64), T(s64), T(s64), T(s64)),
          VALS(&a, &b, &c, &d, &e, &f, &g), &w);
    w = p_align2(a, b, c, d, e, f, g, a);
    CHECK(w % 16 == 0, "the direct call itself is not aligned");
    probe("align 2", (void *)p_align2, T(s64), 8,
          ARGS(T(s64), T(s64), T(s64), T(s64), T(s64), T(s64), T(s64), T(s64)),
          VALS(&a, &b, &c, &d, &e, &f, &g, &a), &w);
}

static void memory(void)
{
    int64_t a = 11, b = -5, cell = 6;
    double d = 3.0;
    S8 in = {100, 200, 300}, w = p_mem_ret(a, in, d, b);
    int32_t k = 9;
    const char *why = "";

    probe("mem ret", (void *)p_mem_ret, &ts8, 4,
          ARGS(T(s64), &ts8, T(f64), T(s64)), VALS(&a, &in, &d, &b), &w);
    {
        int64_t *p = &cell;
        cases++;
        CHECK(meri_abi_call((void *)p_ptr, T(void), 2, ARGS(T(ptr), T(s32)),
                            VALS(&p, &k), NULL, &why) &&
                  cell == 6 * 9 + 1,
              "ptr: the callee did not write through the pointer (%s)", why);
    }
}

/* types the ABI does not carry are refused, never called */
static void refused(void)
{
    static const meri_abi_type *const f1[] = {&meri_abi_s32};
    static const uint32_t o1[] = {2}; /* off its alignment */
    static const meri_abi_type bad = {MERI_ABI_STRUCT, 8, 4, 1, f1, o1};
    static const meri_abi_type *const f2[] = {&meri_abi_s8};
    static const uint32_t o2[] = {0};
    /* 16 bytes, the second eightbyte only padding */
    static const meri_abi_type pad = {MERI_ABI_STRUCT, 16, 8, 1, f2, o2};
    int64_t v = 0;
    const char *why = "";

    cases += 2;
    CHECK(!meri_abi_call((void *)p_none, T(void), 1, ARGS(&bad), VALS(&v), NULL,
                         &why),
          "a field off its alignment was accepted");
    CHECK(!meri_abi_call((void *)p_none, T(void), 1, ARGS(&pad), VALS(&v), NULL,
                         &why),
          "an eightbyte of padding only was accepted");
}

int main(void)
{
    const char *why = "";

    if (!meri_abi_available(&why)) {
        printf("test_abi: skipped: %s\n", why);
        return 0;
    }
    scalars();
    many();
    structs();
    exhaustion();
    memory();
    alignment();
    refused();
    printf("test_abi: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
