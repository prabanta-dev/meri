/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * interp.c - the loop that runs the bytecode, dispatched with goto *.
 *
 * Three stacks, reserved with mmap and touched only as they are used: the
 * registers (the windows of the calls, overlapping on the arguments), the
 * slot areas of the frames, and the frames themselves (where to return).
 * A call that would pass any of them is the trap STACK at the call: the
 * interpreter never recurses in C, so a deep recursion of the program
 * never exhausts the stack of the process.
 *
 * The arithmetic follows the reference interpreter of Limba, operation by
 * operation, on canonical values (vm.h).
 */
#include "vm/code.h"
#include "vm/lower.h"
#include "vm/big.h"
#include "vm/ffi.h"
#include "vm/rt.h"
#include "vm/vm.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

const meri_op_info meri_ops[MERI_OP_COUNT] = {
#define MERI_OP(name, text, format) {text, format},
#include "vm/ops.def"
#undef MERI_OP
};

/* the stacks: 16 Mi registers (128 MiB) and 1 Mi frames; the slots take
   what the budget of the run allows (they count against it), with room
   for the alignment of the frames, reserved up to 1 TiB */
#define REGS ((size_t)1 << 24)
#define FRAMES ((size_t)1 << 20)
#define SLOT_SLACK ((uint64_t)1 << 28)
#define SLOT_MAX ((uint64_t)1 << 40)

typedef struct {
    const uint32_t *pc; /* where the caller goes on */
    const meri_fn *fn;
    uint64_t *base;
    uint8_t *slots, *slot_top;
    uint64_t *dst; /* the caller's register for the result (CALLND), or
                      NULL: then it is the first of the callee's window */
} frame;

static void *reserve(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* ---- the profile of a run (-DMERI_PROFILE, never in a release) ---- */

#ifdef MERI_PROFILE
static uint64_t prof_op[MERI_OP_COUNT];
static uint64_t prof_pair[MERI_OP_COUNT][MERI_OP_COUNT];
static uint64_t prof_rt[LIMBA_RT_COUNT];
static unsigned prof_prev;
#define PROF(op) (prof_op[op]++, prof_pair[prof_prev][op]++, prof_prev = (op))
#define PROF_RT(id) (prof_rt[(id) < LIMBA_RT_COUNT ? (id) : 0]++)

typedef struct {
    uint64_t n;
    unsigned a, b;
} prof_row;

static int prof_by_count(const void *x, const void *y)
{
    const prof_row *p = x, *q = y;
    return p->n < q->n ? 1 : p->n > q->n ? -1 : 0;
}

/* the instructions executed, their pairs and the runtime calls, on
   standard error */
static void prof_dump(void)
{
    static prof_row rows[MERI_OP_COUNT * MERI_OP_COUNT];
    uint64_t total = 0;
    size_t n = 0, i;
    unsigned a, b;

    for (a = 0; a < MERI_OP_COUNT; a++)
        total += prof_op[a];
    fprintf(stderr, "profile: %llu instructions\n", (unsigned long long)total);
    for (a = 0; a < MERI_OP_COUNT; a++)
        if (prof_op[a])
            rows[n++] = (prof_row){prof_op[a], a, 0};
    qsort(rows, n, sizeof(*rows), prof_by_count);
    for (i = 0; i < n && i < 40; i++)
        fprintf(stderr, "op   %12llu %5.1f%%  %s\n",
                (unsigned long long)rows[i].n, 100.0 * rows[i].n / total,
                meri_ops[rows[i].a].text);
    n = 0;
    for (a = 0; a < MERI_OP_COUNT; a++)
        for (b = 0; b < MERI_OP_COUNT; b++)
            if (prof_pair[a][b])
                rows[n++] = (prof_row){prof_pair[a][b], a, b};
    qsort(rows, n, sizeof(*rows), prof_by_count);
    for (i = 0; i < n && i < 40; i++)
        fprintf(stderr, "pair %12llu %5.1f%%  %s %s\n",
                (unsigned long long)rows[i].n, 100.0 * rows[i].n / total,
                meri_ops[rows[i].a].text, meri_ops[rows[i].b].text);
    for (a = 0; a < LIMBA_RT_COUNT; a++)
        if (prof_rt[a])
            fprintf(stderr, "rt   %12llu  %s\n", (unsigned long long)prof_rt[a],
                    limba_rts[a].name);
}
#else
#define PROF(op) ((void)0)
#define PROF_RT(id) ((void)0)
#endif

/* ---- values ---- */

static inline double dv(uint64_t v)
{
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static inline uint64_t db(double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

static inline float fv(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline uint64_t fb(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline uint64_t s32(uint64_t v)
{
    return (uint64_t)(int64_t)(int32_t)(uint32_t)v;
}

/* an integer comparison of canonical values (CHKCC): the unsigned order
   of values sign-extended from one width is the order of that width */
static inline bool int_cc(unsigned cc, uint64_t a, uint64_t b)
{
    switch (cc) {
    case LIMBA_CC_EQ:
        return a == b;
    case LIMBA_CC_NE:
        return a != b;
    case LIMBA_CC_SLT:
        return (int64_t)a < (int64_t)b;
    case LIMBA_CC_SLE:
        return (int64_t)a <= (int64_t)b;
    case LIMBA_CC_SGT:
        return (int64_t)a > (int64_t)b;
    case LIMBA_CC_SGE:
        return (int64_t)a >= (int64_t)b;
    case LIMBA_CC_ULT:
        return a < b;
    case LIMBA_CC_ULE:
        return a <= b;
    case LIMBA_CC_UGT:
        return a > b;
    default: /* LIMBA_CC_UGE: lower.c gives no other */
        return a >= b;
    }
}

static double round_away(double x)
{
    double t = trunc(x), d = x - t;
    if (isnan(x) || isinf(x))
        return x;
    return fabs(d) >= 0.5 ? t + copysign(1.0, x) : t;
}

static float round_awayf(float x)
{
    float t = truncf(x), d = x - t;
    if (isnan(x) || isinf(x))
        return x;
    return fabsf(d) >= 0.5f ? t + copysignf(1.0f, x) : t;
}

/* an integer operation of any width (INTN): 0, or the trap it raises */
static int64_t intn(unsigned op, unsigned bits, uint64_t a, uint64_t b,
                    uint64_t *r)
{
    uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    uint64_t ua = a & mask, ub = b & mask;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    int64_t smin = bits >= 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
    __int128 w;
    limba_id t = bits == 1    ? LIMBA_T_I1
                 : bits == 8  ? LIMBA_T_I8
                 : bits == 16 ? LIMBA_T_I16
                 : bits == 32 ? LIMBA_T_I32
                              : LIMBA_T_I64;

    switch (op) {
    case LIMBA_OP_ADD:
        *r = ua + ub;
        break;
    case LIMBA_OP_SUB:
        *r = ua - ub;
        break;
    case LIMBA_OP_MUL:
        *r = ua * ub;
        break;
    case LIMBA_OP_AND:
        *r = a & b;
        break;
    case LIMBA_OP_OR:
        *r = a | b;
        break;
    case LIMBA_OP_XOR:
        *r = a ^ b;
        break;
    case LIMBA_OP_NOT:
        *r = ~a;
        break;
    case LIMBA_OP_NEG:
        *r = 0 - ua;
        break;
    case LIMBA_OP_SHL:
        *r = ua << (ub % bits);
        break;
    case LIMBA_OP_LSHR:
        *r = ua >> (ub % bits);
        break;
    case LIMBA_OP_ASHR:
        *r = (uint64_t)(sa >> (ub % bits));
        break;
    case LIMBA_OP_UDIV:
    case LIMBA_OP_UREM:
        if (!ub)
            return LIMBA_TRAP_DIVZERO;
        *r = op == LIMBA_OP_UDIV ? ua / ub : ua % ub;
        break;
    case LIMBA_OP_SDIV:
    case LIMBA_OP_SREM:
        if (!sb || (sa == smin && sb == -1))
            return LIMBA_TRAP_DIVZERO;
        *r = (uint64_t)(op == LIMBA_OP_SDIV ? sa / sb : sa % sb);
        break;
    default: /* add.ov, sub.ov, mul.ov */
        w = op == LIMBA_OP_ADDOV   ? (__int128)sa + sb
            : op == LIMBA_OP_SUBOV ? (__int128)sa - sb
                                   : (__int128)sa * sb;
        if (w < smin || w > -(__int128)smin - 1)
            return LIMBA_TRAP_OVERFLOW;
        *r = (uint64_t)(int64_t)w;
    }
    *r = meri_norm(*r, t);
    return 0;
}

/* a comparison of CMPX: X = cc, 1 << 8 for f32 */
static bool cmpx(uint32_t x, uint64_t a, uint64_t b)
{
    unsigned cc = x & 0xff;

    if (limba_cc_is_float(cc)) {
        double p = x >> 8 ? (double)fv(a) : dv(a);
        double q = x >> 8 ? (double)fv(b) : dv(b);
        bool u = isnan(p) || isnan(q);
        switch (cc) {
        case LIMBA_CC_OEQ:
            return !u && p == q;
        case LIMBA_CC_ONE:
            return !u && p != q;
        case LIMBA_CC_OLT:
            return !u && p < q;
        case LIMBA_CC_OLE:
            return !u && p <= q;
        case LIMBA_CC_OGT:
            return !u && p > q;
        case LIMBA_CC_OGE:
            return !u && p >= q;
        case LIMBA_CC_ORD:
            return !u;
        case LIMBA_CC_UNO:
            return u;
        case LIMBA_CC_UEQ:
            return u || p == q;
        case LIMBA_CC_UNE:
            return u || p != q;
        case LIMBA_CC_FULT:
            return u || p < q;
        case LIMBA_CC_FULE:
            return u || p <= q;
        case LIMBA_CC_FUGT:
            return u || p > q;
        default: /* FUGE */
            return u || p >= q;
        }
    }
    switch (cc) { /* canonical integers: 64-bit comparisons agree */
    case LIMBA_CC_EQ:
        return a == b;
    case LIMBA_CC_NE:
        return a != b;
    case LIMBA_CC_SLT:
        return (int64_t)a < (int64_t)b;
    case LIMBA_CC_SLE:
        return (int64_t)a <= (int64_t)b;
    case LIMBA_CC_SGT:
        return (int64_t)a > (int64_t)b;
    case LIMBA_CC_SGE:
        return (int64_t)a >= (int64_t)b;
    case LIMBA_CC_ULT:
        return a < b;
    case LIMBA_CC_ULE:
        return a <= b;
    case LIMBA_CC_UGT:
        return a > b;
    default: /* UGE */
        return a >= b;
    }
}

/* fptosi and fptoui (CONVX): saturating, NaN is 0 */
static uint64_t convx(uint32_t x, uint64_t a)
{
    unsigned op = x & 0xff;
    limba_id from = (x >> 8) & 0xff, to = x >> 16;
    unsigned bits = limba_type_bits(to);
    double d = from == LIMBA_T_F32 ? (double)fv(a) : dv(a), tr = trunc(d);
    uint64_t r;

    if (op == LIMBA_OP_FPTOSI) {
        double lo = -ldexp(1, (int)bits - 1);
        if (isnan(d))
            r = 0;
        else if (tr < lo)
            r = (uint64_t)(int64_t)lo;
        else if (tr >= -lo)
            r = (uint64_t)(bits >= 64 ? INT64_MAX
                                      : ((int64_t)1 << (bits - 1)) - 1);
        else
            r = (uint64_t)(int64_t)tr;
    } else {
        if (isnan(d) || tr <= 0)
            r = 0;
        else if (tr >= ldexp(1, (int)bits))
            r = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        else
            r = (uint64_t)tr;
    }
    return meri_norm(r, to);
}

/* ---- globals ---- */

static void put(void *p, limba_id t, uint64_t v)
{
    switch (t) {
    case LIMBA_T_I1:
    case LIMBA_T_I8: {
        uint8_t x = (uint8_t)v;
        memcpy(p, &x, 1);
        return;
    }
    case LIMBA_T_I16: {
        uint16_t x = (uint16_t)v;
        memcpy(p, &x, 2);
        return;
    }
    case LIMBA_T_I32:
    case LIMBA_T_F32: {
        uint32_t x = (uint32_t)v;
        memcpy(p, &x, 4);
        return;
    }
    default:
        memcpy(p, &v, 8);
    }
}

/* the globals of the module, zeroed, with their initial values; NULL when
   memory is exhausted */
static void **globals_new(meri_state *s)
{
    const limba_module *m = s->p->m;
    void **g = calloc((size_t)m->nglobals + 1, sizeof(void *));
    uint32_t i;

    if (!g)
        return NULL;
    for (i = 0; i < m->nglobals; i++) {
        const limba_global *gl = &m->globals[i];
        const limba_type *ty = &m->types[gl->type];
        size_t align = ty->align < 8 ? 8 : ty->align;
        size_t size = ty->size ? ty->size : 1;
        void *p;
        size = (size + align - 1) / align * align;
        if (!meri_state_take(s, size) || posix_memalign(&p, align, size)) {
            while (i-- > 0)
                free(g[i]);
            free(g);
            return NULL;
        }
        memset(p, 0, size);
        g[i] = p;
        if (gl->init == LIMBA_INIT_INT) {
            put(p, gl->type, meri_norm((uint64_t)gl->value, gl->type));
        } else if (gl->init == LIMBA_INIT_FLOAT) {
            put(p, gl->type,
                gl->type == LIMBA_T_F32 ? (uint64_t)(uint32_t)gl->value
                                        : (uint64_t)gl->value);
        } else if (gl->init == LIMBA_INIT_STR) {
            size_t n;
            const char *text = limba_str(m, (limba_id)gl->value, &n);
            meri_str *x = meri_state_immortal(s, text, n);
            if (!x) {
                for (;; i--) {
                    free(g[i]);
                    if (!i)
                        break;
                }
                free(g);
                return NULL;
            }
            put(p, LIMBA_T_STR, meri_str_value(x));
        }
    }
    return g;
}

static limba_id find_entry(const limba_module *m, const char *entry)
{
    size_t n = strlen(entry), k;
    uint32_t i;

    for (i = 0; i < m->nfuncs; i++) {
        const char *s = limba_str(m, m->funcs[i].name, &k);
        if (k == n && !memcmp(s, entry, n))
            return i;
    }
    return LIMBA_NONE;
}

/* ---- the loop ---- */

uint64_t meri_default_memory(void)
{
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);

    if (pages <= 0 || size <= 0)
        return (uint64_t)1 << 30;
    return (uint64_t)pages * (uint64_t)size / 2;
}

/* p aligned up to a (a power of two); NULL past end */
static uint8_t *align_up(uint8_t *p, uint64_t a, const uint8_t *end)
{
    uintptr_t q = ((uintptr_t)p + (uintptr_t)a - 1) & ~(uintptr_t)(a - 1);

    return q < (uintptr_t)p || q > (uintptr_t)end ? NULL : (uint8_t *)q;
}

static void run(meri_state *s, const meri_fn *entry, void **globals,
                uint64_t *regs, uint8_t *slotmem, size_t nslotmem,
                frame *frames, meri_result *r)
{
    static const void *const disp[MERI_OP_COUNT] = {
#define MERI_OP(name, text, format) &&op_##name,
#include "vm/ops.def"
#undef MERI_OP
    };
    const meri_program *p = s->p;
    uint64_t *const regs_end = regs + REGS;
    uint8_t *const slots_end = slotmem + nslotmem;
    const meri_fn *fn = entry, *callee;
    const uint32_t *pc, *ip;
    const uint64_t *k;
    uint64_t *base = regs;
    uint8_t *slots, *slot_top;
    size_t depth = 0;
    int64_t code = 0;
    uint64_t result;
    uint32_t w, x, ca = 0;
    uint64_t *dst = NULL; /* of the next call: see frame */
    bool nd = false;

    ip = fn->code;
    code = LIMBA_TRAP_NOMEM;
    if (!meri_state_take(s, fn->slot_size))
        goto trap;
    slots = align_up(slotmem, fn->slot_align, slots_end);
    code = LIMBA_TRAP_STACK;
    if (fn->nregs > REGS || !slots ||
        (uint64_t)(slots_end - slots) < fn->slot_size)
        goto trap;
    memset(slots, 0, fn->slot_size);
    slot_top = slots + fn->slot_size;
    pc = fn->code;
    k = fn->k;

#define NEXT                                                                   \
    do {                                                                       \
        w = *pc++;                                                             \
        PROF(MERI_W_OP(w));                                                    \
        goto *disp[MERI_W_OP(w)];                                              \
    } while (0)
#define RA base[MERI_W_A(w)]
#define RB base[MERI_W_B(w)]
#define RC base[MERI_W_C(w)]
#define TRAP(c)                                                                \
    do {                                                                       \
        code = (c);                                                            \
        goto trap;                                                             \
    } while (0)
/* the JMP word after a test: taken when cond is the k of the test */
#define JUMP_IF(cond, kk)                                                      \
    do {                                                                       \
        uint32_t j = *pc++;                                                    \
        if ((cond) == (kk))                                                    \
            pc += MERI_W_SJ(j);                                                \
        NEXT;                                                                  \
    } while (0)

    NEXT;

op_MOVE:
    RA = RB;
    NEXT;
op_LOADI:
    RA = (uint64_t)(int64_t)MERI_W_SBX(w);
    NEXT;
op_LOADK:
    RA = k[MERI_W_BX(w)];
    NEXT;
op_LOADS:
    RA = meri_str_value(p->strs[MERI_W_BX(w)]);
    NEXT;

op_ADD:
    RA = RB + RC;
    NEXT;
op_SUB:
    RA = RB - RC;
    NEXT;
op_MUL:
    RA = RB * RC;
    NEXT;
op_ADDOV: {
    ip = pc - 1;
    int64_t v;
    if (__builtin_add_overflow((int64_t)RB, (int64_t)RC, &v))
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_SUBOV: {
    ip = pc - 1;
    int64_t v;
    if (__builtin_sub_overflow((int64_t)RB, (int64_t)RC, &v))
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_MULOV: {
    ip = pc - 1;
    int64_t v;
    if (__builtin_mul_overflow((int64_t)RB, (int64_t)RC, &v))
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_SDIV: {
    ip = pc - 1;
    int64_t a = (int64_t)RB, b = (int64_t)RC;
    if (!b || (a == INT64_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = (uint64_t)(a / b);
    NEXT;
}
op_UDIV:
    ip = pc - 1;
    if (!RC)
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = RB / RC;
    NEXT;
op_SREM: {
    ip = pc - 1;
    int64_t a = (int64_t)RB, b = (int64_t)RC;
    if (!b || (a == INT64_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = (uint64_t)(a % b);
    NEXT;
}
op_UREM:
    ip = pc - 1;
    if (!RC)
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = RB % RC;
    NEXT;
op_NEG:
    RA = 0 - RB;
    NEXT;
op_SHL:
    RA = RB << (RC & 63);
    NEXT;
op_LSHR:
    RA = RB >> (RC & 63);
    NEXT;
op_ASHR:
    RA = (uint64_t)((int64_t)RB >> (RC & 63));
    NEXT;

op_ADD32:
    RA = s32(RB + RC);
    NEXT;
op_SUB32:
    RA = s32(RB - RC);
    NEXT;
op_MUL32:
    RA = s32((uint64_t)((uint32_t)RB * (uint32_t)RC));
    NEXT;
op_ADDOV32: {
    ip = pc - 1;
    int64_t v = (int64_t)RB + (int64_t)RC;
    if (v < INT32_MIN || v > INT32_MAX)
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_SUBOV32: {
    ip = pc - 1;
    int64_t v = (int64_t)RB - (int64_t)RC;
    if (v < INT32_MIN || v > INT32_MAX)
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_MULOV32: {
    ip = pc - 1;
    int64_t v = (int64_t)RB * (int64_t)RC; /* two i32: no overflow */
    if (v < INT32_MIN || v > INT32_MAX)
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_SDIV32: {
    ip = pc - 1;
    int32_t a = (int32_t)RB, b = (int32_t)RC;
    if (!b || (a == INT32_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = (uint64_t)(int64_t)(a / b);
    NEXT;
}
op_UDIV32: {
    ip = pc - 1;
    uint32_t a = (uint32_t)RB, b = (uint32_t)RC;
    if (!b)
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = s32(a / b);
    NEXT;
}
op_SREM32: {
    ip = pc - 1;
    int32_t a = (int32_t)RB, b = (int32_t)RC;
    if (!b || (a == INT32_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = (uint64_t)(int64_t)(a % b);
    NEXT;
}
op_UREM32: {
    ip = pc - 1;
    uint32_t a = (uint32_t)RB, b = (uint32_t)RC;
    if (!b)
        TRAP(LIMBA_TRAP_DIVZERO);
    RA = s32(a % b);
    NEXT;
}
op_NEG32:
    RA = s32(0 - RB);
    NEXT;
op_SHL32:
    RA = s32((uint64_t)((uint32_t)RB << ((uint32_t)RC % 32)));
    NEXT;
op_LSHR32:
    RA = s32((uint64_t)((uint32_t)RB >> ((uint32_t)RC % 32)));
    NEXT;
op_ASHR32:
    RA = (uint64_t)((int64_t)RB >> ((uint32_t)RC % 32));
    NEXT;

op_INTN: {
    ip = pc - 1;
    uint64_t v;
    int64_t c;
    x = *pc++;
    c = intn(x & 0xff, (x >> 8) & 0xff, RB, RC, &v);
    if (c)
        TRAP(c);
    RA = v;
    NEXT;
}

op_AND:
    RA = RB & RC;
    NEXT;
op_OR:
    RA = RB | RC;
    NEXT;
op_XOR:
    RA = RB ^ RC;
    NEXT;
op_NOT:
    RA = ~RB;
    NEXT;

op_FADD:
    RA = db(dv(RB) + dv(RC));
    NEXT;
op_FSUB:
    RA = db(dv(RB) - dv(RC));
    NEXT;
op_FMUL:
    RA = db(dv(RB) * dv(RC));
    NEXT;
op_FDIV:
    RA = db(dv(RB) / dv(RC));
    NEXT;
op_FNEG:
    RA = RB ^ (1ull << 63); /* the sign: a NaN keeps the rest */
    NEXT;
op_FMA: {
    double c;
    x = *pc++;
    c = dv(base[x & 0xff]);
    RA = db(fma(dv(RB), dv(RC), c));
    NEXT;
}
op_FROUND:
    RA = db(nearbyint(dv(RB)));
    NEXT;
op_FROUNDA:
    RA = db(round_away(dv(RB)));
    NEXT;
op_FADDF:
    RA = fb(fv(RB) + fv(RC));
    NEXT;
op_FSUBF:
    RA = fb(fv(RB) - fv(RC));
    NEXT;
op_FMULF:
    RA = fb(fv(RB) * fv(RC));
    NEXT;
op_FDIVF:
    RA = fb(fv(RB) / fv(RC));
    NEXT;
op_FNEGF:
    RA = RB ^ 0x80000000u;
    NEXT;
op_FMAF: {
    float c;
    x = *pc++;
    c = fv(base[x & 0xff]);
    RA = fb(fmaf(fv(RB), fv(RC), c));
    NEXT;
}
op_FROUNDF:
    RA = fb(nearbyintf(fv(RB)));
    NEXT;
op_FROUNDAF:
    RA = fb(round_awayf(fv(RB)));
    NEXT;

op_EQ:
    RA = RB == RC;
    NEXT;
op_NE:
    RA = RB != RC;
    NEXT;
op_LT:
    RA = (int64_t)RB < (int64_t)RC;
    NEXT;
op_LE:
    RA = (int64_t)RB <= (int64_t)RC;
    NEXT;
op_LTU:
    RA = RB < RC;
    NEXT;
op_LEU:
    RA = RB <= RC;
    NEXT;
op_FEQ:
    RA = dv(RB) == dv(RC);
    NEXT;
op_FLT:
    RA = dv(RB) < dv(RC);
    NEXT;
op_FLE:
    RA = dv(RB) <= dv(RC);
    NEXT;
op_CMPX:
    x = *pc++;
    RA = cmpx(x, RB, RC);
    NEXT;
op_SELECT: {
    uint64_t v;
    x = *pc++;
    v = RB ? RC : base[x & 0xff];
    RA = v;
    NEXT;
}

op_SEXT8:
    RA = (uint64_t)(int64_t)(int8_t)(uint8_t)RB;
    NEXT;
op_SEXT16:
    RA = (uint64_t)(int64_t)(int16_t)(uint16_t)RB;
    NEXT;
op_SEXT32:
    RA = s32(RB);
    NEXT;
op_ZEXT1:
    RA = RB & 1;
    NEXT;
op_ZEXT8:
    RA = RB & 0xff;
    NEXT;
op_ZEXT16:
    RA = RB & 0xffff;
    NEXT;
op_ZEXT32:
    RA = RB & 0xffffffffu;
    NEXT;
op_I2D:
    RA = db((double)(int64_t)RB);
    NEXT;
op_I2F:
    RA = fb((float)(int64_t)RB);
    NEXT;
op_U2D:
    RA = db((double)RB);
    NEXT;
op_U2F:
    RA = fb((float)RB);
    NEXT;
op_D2F:
    RA = fb((float)dv(RB));
    NEXT;
op_F2D:
    RA = db((double)fv(RB));
    NEXT;
op_CONVX:
    x = *pc++;
    RA = convx(x, RB);
    NEXT;

op_LD1:
    RA = *(const uint8_t *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)) & 1;
    NEXT;
op_LD8:
    RA = (uint64_t)(int64_t)*(const int8_t *)(uintptr_t)MERI_ADDR(RB +
                                                                  MERI_W_C(w));
    NEXT;
op_LD16: {
    int16_t v;
    memcpy(&v, (const void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), sizeof(v));
    RA = (uint64_t)(int64_t)v;
    NEXT;
}
op_LD32: {
    int32_t v;
    memcpy(&v, (const void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), sizeof(v));
    RA = (uint64_t)(int64_t)v;
    NEXT;
}
op_LDF32: {
    uint32_t v;
    memcpy(&v, (const void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), sizeof(v));
    RA = v;
    NEXT;
}
op_LD64: {
    uint64_t v;
    memcpy(&v, (const void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), sizeof(v));
    RA = v;
    NEXT;
}
op_ST8: {
    uint8_t v = (uint8_t)RA;
    memcpy((void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), &v, sizeof(v));
    NEXT;
}
op_ST16: {
    uint16_t v = (uint16_t)RA;
    memcpy((void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), &v, sizeof(v));
    NEXT;
}
op_ST32: {
    uint32_t v = (uint32_t)RA;
    memcpy((void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), &v, sizeof(v));
    NEXT;
}
op_ST64: {
    uint64_t v = RA;
    memcpy((void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), &v, sizeof(v));
    NEXT;
}
op_ADDR:
    x = *pc++;
    RA = RB + RC * k[x] + k[x + 1];
    NEXT;
op_SLOT:
    RA = (uint64_t)(uintptr_t)(slots + fn->slot_off[MERI_W_BX(w)]);
    NEXT;
op_GADDR:
    RA = (uint64_t)(uintptr_t)globals[MERI_W_BX(w)];
    NEXT;
op_FADDR:
    RA = (uint64_t)(uintptr_t)&p->fns[MERI_W_BX(w)];
    NEXT;
op_MEMCPY:
    memmove((void *)(uintptr_t)MERI_ADDR(RA),
            (const void *)(uintptr_t)MERI_ADDR(RB), (size_t)RC);
    NEXT;
op_MEMSET:
    memset((void *)(uintptr_t)MERI_ADDR(RA), (int)(uint8_t)RB, (size_t)RC);
    NEXT;
op_RC: {
    ip = pc - 1;
    int64_t n = (int64_t)RB;
    uint64_t size;
    x = *pc++;
    size = p->m->types[x].size;
    if (n < 0 || (n && size > UINT64_MAX / (uint64_t)n))
        TRAP(LIMBA_TRAP_RANGE);
    meri_state_rc(s, RA, x, (uint64_t)n, MERI_W_C(w) ? -1 : 1);
    NEXT;
}
op_SRETAIN:
    meri_str_retain(RA);
    NEXT;
op_SRELEASE:
    meri_state_release(s, RA);
    NEXT;
op_RRELEASE:
    meri_big_release(s, RA);
    NEXT;
op_STSR: {
    void *q = (void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w));
    uint64_t v = RA, old;
    memcpy(&old, q, sizeof(old));
    meri_str_retain(v);
    memcpy(q, &v, sizeof(v));
    meri_big_release(s, old);
    NEXT;
}
op_LDS: {
    uint64_t v;
    memcpy(&v, (const void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w)), sizeof(v));
    meri_str_retain(v);
    RA = v;
    NEXT;
}
op_STS: {
    void *q = (void *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w));
    uint64_t v = RA, old;
    memcpy(&old, q, sizeof(old));
    meri_str_retain(v);
    memcpy(q, &v, sizeof(v));
    meri_state_release(s, old);
    NEXT;
}
op_RELSLOTS: {
    uint32_t i;
    for (i = 0; i < fn->nrel_slots; i++) {
        uint32_t k2 = fn->rel_slots[i];
        meri_state_rc(s, (uint64_t)(uintptr_t)(slots + fn->slot_off[k2]),
                      fn->rel_types[i], 1, -1);
    }
    NEXT;
}

op_CALL:
    ip = pc - 1;
    callee = &p->fns[MERI_W_BX(w)];
    ca = MERI_W_A(w);
    goto call;
op_CALLN:
    ip = pc - 1;
    callee = &p->fns[MERI_W_BX(w)];
    ca = MERI_W_A(w);
    nd = false;
    goto copy_args;
op_CALLND:
    ip = pc - 1;
    callee = &p->fns[MERI_W_BX(w)];
    ca = MERI_W_A(w);
    nd = true;
    goto copy_args;
op_CALLRTN:
    ip = pc - 1;
    callee = NULL; /* a runtime function */
    ca = MERI_W_A(w);
copy_args: {
    /* the registers of the arguments, a byte each after the count */
    uint32_t n = *pc & 0xff, i2, word = *pc++;
    uint64_t *to = base + ca;
    for (i2 = 0; i2 < n; i2++) {
        uint32_t kk = i2 + 1;
        if (kk % 4 == 0)
            word = *pc++;
        to[i2] = base[(word >> (8 * (kk % 4))) & 0xff];
    }
    if (callee) {
        if (nd) /* the register of the result, in the word after */
            dst = base + *pc++;
        goto call;
    }
    PROF_RT(MERI_W_BX(w));
    if (!meri_rt_call(s, MERI_W_BX(w), to)) {
        r->status = s->status;
        r->code = s->code;
        goto stop;
    }
    NEXT;
}
op_CALLW:
    ip = pc - 1;
    callee = &p->fns[MERI_W_BX(w)];
    ca = *pc++;
    goto call;
op_CALLIW:
    ip = pc - 1;
    ca = MERI_W_A(w) | MERI_W_C(w) << 8;
    goto calli;
op_MOVEW:
    x = *pc++;
    base[x & 0xffff] = base[x >> 16];
    NEXT;
op_CALLRTW:
    ip = pc - 1;
    x = *pc++;
    if (!meri_rt_call(s, MERI_W_BX(w), &base[x])) {
        r->status = s->status;
        r->code = s->code;
        goto stop;
    }
    NEXT;
op_CALLI:
    ip = pc - 1;
    ca = MERI_W_A(w);
calli: {
    uintptr_t v = (uintptr_t)RB, b0 = (uintptr_t)p->fns;
    x = *pc++;
    if (v < b0 || (v - b0) % sizeof(meri_fn) ||
        (v - b0) / sizeof(meri_fn) >= p->nfns ||
        ((const meri_fn *)v)->type != (limba_id)x) {
        r->status = MERI_BADCALL;
        goto stop;
    }
    callee = (const meri_fn *)v;
    goto call;
}
call: {
    uint64_t *nb = base + ca;
    uint8_t *ns = align_up(slot_top, callee->slot_align, slots_end);
    /* the slots are memory of the program: past the budget, NOMEM */
    if (callee->slot_size && !meri_state_take(s, callee->slot_size))
        TRAP(LIMBA_TRAP_NOMEM);
    if (depth + 1 >= FRAMES || (size_t)(regs_end - nb) < callee->nregs || !ns ||
        (uint64_t)(slots_end - ns) < callee->slot_size) {
        meri_state_give(s, callee->slot_size);
        TRAP(LIMBA_TRAP_STACK);
    }
    frames[depth++] = (frame){pc, fn, base, slots, slot_top, dst};
    dst = NULL;
    memset(ns, 0, callee->slot_size);
    fn = callee;
    base = nb;
    slots = ns;
    slot_top = ns + fn->slot_size;
    k = fn->k;
    pc = fn->code;
    NEXT;
}
op_CALLRT:
    ip = pc - 1;
    PROF_RT(MERI_W_BX(w));
    if (!meri_rt_call(s, MERI_W_BX(w), &RA)) {
        r->status = s->status;
        r->code = s->code;
        goto stop;
    }
    NEXT;
op_CALLX: /* the arguments from A, as a call's: ffi.h */
    ip = pc - 1;
    if (!meri_ffi_call(p, MERI_W_BX(w), base + MERI_W_A(w), s)) {
        r->status = s->status;
        r->code = s->code;
        goto stop;
    }
    NEXT;
op_RET:
    result = RA;
    base[0] = result;
    goto ret;
op_RET0:
    result = 0;
ret:
    if (fn->slot_size)
        meri_state_give(s, fn->slot_size);
    if (!depth) {
        r->status = MERI_OK;
        r->ret = result;
        return;
    }
    depth--;
    pc = frames[depth].pc;
    fn = frames[depth].fn;
    base = frames[depth].base;
    slots = frames[depth].slots;
    slot_top = frames[depth].slot_top;
    if (frames[depth].dst)
        *frames[depth].dst = result;
    k = fn->k;
    NEXT;

op_JMP:
    pc += MERI_W_SJ(w);
    NEXT;
op_TEST:
    JUMP_IF(RA != 0, MERI_W_B(w));
op_JEQ:
    JUMP_IF(RA == RB, MERI_W_C(w));
op_JLT:
    JUMP_IF((int64_t)RA < (int64_t)RB, MERI_W_C(w));
op_JLE:
    JUMP_IF((int64_t)RA <= (int64_t)RB, MERI_W_C(w));
op_JLTU:
    JUMP_IF(RA < RB, MERI_W_C(w));
op_JLEU:
    JUMP_IF(RA <= RB, MERI_W_C(w));
op_JFEQ:
    JUMP_IF(dv(RA) == dv(RB), MERI_W_C(w));
op_JFLT:
    JUMP_IF(dv(RA) < dv(RB), MERI_W_C(w));
op_JFLE:
    JUMP_IF(dv(RA) <= dv(RB), MERI_W_C(w));
op_TRAP:
    ip = pc - 1;
    TRAP((int64_t)k[MERI_W_BX(w)]);
op_CHECK:
    ip = pc - 1;
    if (!RA)
        TRAP((int64_t)k[MERI_W_BX(w)]);
    NEXT;
op_FSQRT:
    RA = db(sqrt(dv(RB)));
    NEXT;
op_FMATH: {
    double v = dv(RB);
    switch (MERI_W_C(w)) {
    case LIMBA_RT_MATH_SIN:
        v = sin(v);
        break;
    case LIMBA_RT_MATH_COS:
        v = cos(v);
        break;
    case LIMBA_RT_MATH_TAN:
        v = tan(v);
        break;
    case LIMBA_RT_MATH_ATAN:
        v = atan(v);
        break;
    case LIMBA_RT_MATH_EXP:
        v = exp(v);
        break;
    case LIMBA_RT_MATH_LN:
        v = log(v);
        break;
    case LIMBA_RT_MATH_TRUNC:
        v = trunc(v);
        break;
    case LIMBA_RT_MATH_FLOOR:
        v = floor(v);
        break;
    default: /* ceil */
        v = ceil(v);
    }
    RA = db(v);
    NEXT;
}
op_CHKLIVE:
    ip = pc - 1;
    if (!meri_heap_live(&s->heap, RA))
        TRAP((int64_t)k[MERI_W_BX(w)]);
    NEXT;
op_CHKR: {
    ip = pc - 1;
    int64_t v = (int64_t)RA;
    x = *pc++;
    if (!((int64_t)RB <= v && v <= (int64_t)RC))
        TRAP((int64_t)k[x]);
    NEXT;
}
op_CHKRK: {
    ip = pc - 1;
    int64_t v = (int64_t)RA;
    x = *pc++;
    if (!((int64_t)k[x] <= v && v <= (int64_t)k[x + 1]))
        TRAP((int64_t)MERI_W_B(w));
    NEXT;
}
#define XADDR ((void *)(uintptr_t)MERI_ADDR(RB + RC * k[x] + k[x + 1]))
op_LDX1:
    x = *pc++;
    RA = *(const uint8_t *)XADDR & 1;
    NEXT;
op_LDX8:
    x = *pc++;
    RA = (uint64_t)(int64_t)*(const int8_t *)XADDR;
    NEXT;
op_LDX16: {
    int16_t v;
    x = *pc++;
    memcpy(&v, XADDR, sizeof(v));
    RA = (uint64_t)(int64_t)v;
    NEXT;
}
op_LDX32: {
    int32_t v;
    x = *pc++;
    memcpy(&v, XADDR, sizeof(v));
    RA = (uint64_t)(int64_t)v;
    NEXT;
}
op_LDXF32: {
    uint32_t v;
    x = *pc++;
    memcpy(&v, XADDR, sizeof(v));
    RA = v;
    NEXT;
}
op_LDX64: {
    uint64_t v;
    x = *pc++;
    memcpy(&v, XADDR, sizeof(v));
    RA = v;
    NEXT;
}
op_STX8: {
    uint8_t v = (uint8_t)RA;
    x = *pc++;
    memcpy(XADDR, &v, sizeof(v));
    NEXT;
}
op_STX16: {
    uint16_t v = (uint16_t)RA;
    x = *pc++;
    memcpy(XADDR, &v, sizeof(v));
    NEXT;
}
op_STX32: {
    uint32_t v = (uint32_t)RA;
    x = *pc++;
    memcpy(XADDR, &v, sizeof(v));
    NEXT;
}
op_STX64: {
    uint64_t v = RA;
    x = *pc++;
    memcpy(XADDR, &v, sizeof(v));
    NEXT;
}
#undef XADDR
op_STRPTR:
    RA = (uint64_t)(uintptr_t)meri_str_of(RB)->data;
    NEXT;
op_STRLEN:
    RA = meri_str_of(RB)->len;
    NEXT;
op_ADDI:
    RA = RB + MERI_W_C(w);
    NEXT;
op_LOOP:
    if (RA != RB) {
        RA += RC;
        pc += 1 + MERI_W_SJ(*pc);
        NEXT;
    }
    pc++;
    NEXT;
op_LOOP32:
    if (RA != RB) {
        RA = s32(RA + RC);
        pc += 1 + MERI_W_SJ(*pc);
        NEXT;
    }
    pc++;
    NEXT;
op_ALLOC: {
    uint64_t n = RB, c = meri_heap_charge(n), q;
    ip = pc - 1;
    if ((int64_t)n < 0 || !meri_state_take(s, c))
        TRAP(LIMBA_TRAP_NOMEM);
    q = meri_heap_alloc(&s->heap, n);
    if (!q) {
        meri_state_give(s, c);
        TRAP(LIMBA_TRAP_NOMEM);
    }
    RA = q;
    NEXT;
}
op_CHKNL:
    ip = pc - 1;
    if (!RA)
        TRAP((int64_t)k[MERI_W_BX(w)]);
    if (!meri_heap_live(&s->heap, RA))
        TRAP((int64_t)k[MERI_W_BX(w) + 1]);
    NEXT;
op_LOADKW:
    RA = k[*pc++];
    NEXT;
op_TRAPW:
    ip = pc - 1;
    TRAP((int64_t)k[*pc]);
op_CHECKW:
    ip = pc - 1;
    x = *pc++;
    if (!RA)
        TRAP((int64_t)k[x]);
    NEXT;
op_CHKLIVEW:
    ip = pc - 1;
    x = *pc++;
    if (!meri_heap_live(&s->heap, RA))
        TRAP((int64_t)k[x]);
    NEXT;
op_CHKNLW:
    ip = pc - 1;
    x = *pc++;
    if (!RA)
        TRAP((int64_t)k[x]);
    if (!meri_heap_live(&s->heap, RA))
        TRAP((int64_t)k[x + 1]);
    NEXT;
op_CHKRS: {
    int64_t v = (int64_t)RA;
    ip = pc - 1;
    x = *pc++;
    if (!((int64_t)RB <= v && v <= (int64_t)RC))
        TRAP((int64_t)k[x & 0xffff]);
    base[x >> 16] = RA - RB;
    NEXT;
}
op_CHKCC:
    ip = pc - 1;
    x = *pc++;
    if (!int_cc(MERI_W_C(w), RA, RB))
        TRAP((int64_t)k[x]);
    NEXT;
op_MEMCPYK: {
    uint8_t *d = (uint8_t *)(uintptr_t)MERI_ADDR(RA);
    const uint8_t *q = (const uint8_t *)(uintptr_t)MERI_ADDR(RB);
    uint64_t a0, a1;
    switch (MERI_W_C(w)) {
    case 8:
        memcpy(&a0, q, 8);
        memcpy(d, &a0, 8);
        break;
    case 16: /* both read before a byte is written: as memmove */
        memcpy(&a0, q, 8);
        memcpy(&a1, q + 8, 8);
        memcpy(d, &a0, 8);
        memcpy(d + 8, &a1, 8);
        break;
    default:
        memmove(d, q, MERI_W_C(w));
    }
    NEXT;
}
op_MEMSETK: {
    uint8_t *d = (uint8_t *)(uintptr_t)MERI_ADDR(RA);
    uint64_t v = 0x0101010101010101ull * (uint8_t)RB;
    switch (MERI_W_C(w)) {
    case 8:
        memcpy(d, &v, 8);
        break;
    case 16:
        memcpy(d, &v, 8);
        memcpy(d + 8, &v, 8);
        break;
    default:
        memset(d, (int)(uint8_t)RB, MERI_W_C(w));
    }
    NEXT;
}
op_LIVE:
    RA = meri_heap_live(&s->heap, RB);
    NEXT;
op_ADDK:
    RA = RB + (uint64_t)(int64_t)MERI_W_SC(w);
    NEXT;
op_ADDK32:
    RA = s32(RB + (uint64_t)(int64_t)MERI_W_SC(w));
    NEXT;
op_ADDOVK: {
    int64_t v;
    ip = pc - 1;
    if (__builtin_add_overflow((int64_t)RB, (int64_t)MERI_W_SC(w), &v))
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_ADDOVK32: {
    int64_t v = (int64_t)RB + MERI_W_SC(w);
    ip = pc - 1;
    if (v < INT32_MIN || v > INT32_MAX)
        TRAP(LIMBA_TRAP_OVERFLOW);
    RA = (uint64_t)v;
    NEXT;
}
op_JEQK:
    JUMP_IF((int64_t)RA == MERI_W_SB(w), MERI_W_C(w));
op_JLTK:
    JUMP_IF((int64_t)RA < MERI_W_SB(w), MERI_W_C(w));
op_JLEK:
    JUMP_IF((int64_t)RA <= MERI_W_SB(w), MERI_W_C(w));
op_RETK:
    result = (uint64_t)(int64_t)MERI_W_SBX(w);
    base[0] = result;
    goto ret;
op_ALLOCK: {
    uint64_t n = MERI_W_BX(w), c = meri_heap_charge(n), q;
    ip = pc - 1;
    if (!meri_state_take(s, c))
        TRAP(LIMBA_TRAP_NOMEM);
    q = meri_heap_alloc(&s->heap, n);
    if (!q) {
        meri_state_give(s, c);
        TRAP(LIMBA_TRAP_NOMEM);
    }
    RA = q;
    NEXT;
}
op_FREE: {
    uint64_t size;
    ip = pc - 1;
    if (!meri_heap_free(&s->heap, RA, &size))
        TRAP(LIMBA_TRAP_INVALID_FREE);
    meri_state_give(s, size);
    NEXT;
}
op_SMOD: {
    int64_t a = (int64_t)RB, b = (int64_t)RC, m;
    ip = pc - 1;
    if (!b || (a == INT64_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    m = a % b;
    if (m != 0 && (m ^ b) < 0)
        m += b;
    RA = (uint64_t)m;
    NEXT;
}
op_SMOD32: {
    int32_t a = (int32_t)RB, b = (int32_t)RC, m;
    ip = pc - 1;
    if (!b || (a == INT32_MIN && b == -1))
        TRAP(LIMBA_TRAP_DIVZERO);
    m = a % b;
    if (m != 0 && (m ^ b) < 0)
        m += b;
    RA = (uint64_t)(int64_t)m;
    NEXT;
}
op_FMADD:
    x = *pc++;
    RA = db(dv(RB) * dv(RC) + dv(base[x]));
    NEXT;
op_FMADDR:
    x = *pc++;
    RA = db(dv(base[x]) + dv(RB) * dv(RC));
    NEXT;
op_FMSUB:
    x = *pc++;
    RA = db(dv(RB) * dv(RC) - dv(base[x]));
    NEXT;
op_FMSUBR:
    x = *pc++;
    RA = db(dv(base[x]) - dv(RB) * dv(RC));
    NEXT;
op_FMUL3:
    x = *pc++;
    RA = db(dv(RB) * dv(RC) * dv(base[x]));
    NEXT;
op_FMUL3R:
    x = *pc++;
    RA = db(dv(base[x]) * (dv(RB) * dv(RC)));
    NEXT;
op_LOOPD:
    if (RA != RB) {
        RA -= RC;
        pc += 1 + MERI_W_SJ(*pc);
        NEXT;
    }
    pc++;
    NEXT;
op_LOOPD32:
    if (RA != RB) {
        RA = s32(RA - RC);
        pc += 1 + MERI_W_SJ(*pc);
        NEXT;
    }
    pc++;
    NEXT;
op_LDU8:
    RA = *(const uint8_t *)(uintptr_t)MERI_ADDR(RB + MERI_W_C(w));
    NEXT;
op_LDXU8:
    x = *pc++;
    RA = *(const uint8_t *)(uintptr_t)MERI_ADDR(RB + RC * k[x] + k[x + 1]);
    NEXT;
op_SDIVK: {
    int64_t a = (int64_t)RB, d = MERI_W_SC(w);
    if (!(d & (d - 1))) { /* toward zero: a negative a gets d - 1 first */
        unsigned sh = (unsigned)__builtin_ctzll((uint64_t)d);
        RA = (uint64_t)((a + ((a >> 63) & (d - 1))) >> sh);
    } else {
        RA = (uint64_t)(a / d);
    }
    NEXT;
}
op_CHKADDR: {
    int64_t v = (int64_t)RB;
    uint32_t y;
    ip = pc - 1;
    x = *pc++;
    y = *pc++;
    if (!((int64_t)base[x & 0xff] <= v && v <= (int64_t)base[x >> 8 & 0xff]))
        TRAP((int64_t)k[x >> 16]);
    RA = RC + RB * k[y] + k[y + 1];
    NEXT;
}
op_CHKADDRS: {
    int64_t v = (int64_t)RB;
    uint64_t lo;
    uint32_t y;
    ip = pc - 1;
    x = *pc++;
    y = *pc++;
    lo = base[x & 0xff];
    if (!((int64_t)lo <= v && v <= (int64_t)base[x >> 8 & 0xff]))
        TRAP((int64_t)k[x >> 16]);
    RA = RC + (RB - lo) * k[y] + k[y + 1];
    NEXT;
}
op_PUTC: {
    uint32_t c = (uint32_t)RA;
    char b8[4];
    unsigned nb8, i3;
    /* UTF-8, as the runtime writes it (rt.c) */
    if (c < 0x80) {
        if (fputc((int)c, s->env->out) == EOF)
            goto out_error;
        NEXT;
    }
    nb8 = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    for (i3 = nb8; i3-- > 1;) {
        b8[i3] = (char)(0x80 | (c & 0x3f));
        c >>= 6;
    }
    b8[0] = (char)((0xf00 >> nb8) | c);
    if (fwrite(b8, 1, nb8, s->env->out) != nb8)
        goto out_error;
    NEXT;
}
op_PUTB:
    if (fputc((int)(uint8_t)RA, s->env->out) == EOF)
        goto out_error;
    NEXT;
out_error: /* no position: the write that finds it may be a later one */
    ip = pc - 1;
    meri_rt_out_error(s, errno);
    r->status = s->status;
    r->code = s->code;
    goto stop;
op_UNREACH:
    ip = pc - 1;
    r->status = MERI_UNREACHABLE;
    goto stop;

trap:
    r->status = MERI_TRAP;
    r->code = code;
stop:
    r->pos = fn->pos && !s->nopos ? fn->pos[ip - fn->code] : 0;
#undef NEXT
#undef RA
#undef RB
#undef RC
#undef TRAP
#undef JUMP_IF
}

void meri_run(const meri_program *p, const char *entry, const meri_env *env,
              meri_result *r)
{
    meri_state s = {.p = p, .env = env, .status = MERI_OK};
    limba_id fid = find_entry(p->m, entry);
    void **globals = NULL;
    uint64_t *regs = NULL;
    uint8_t *slotmem = NULL;
    frame *frames = NULL;
    size_t nslotmem;
    size_t i;

    s.heap.tagged = p->m->memory == LIMBA_MEM_STRICT;
    s.budget = env->max_memory ? env->max_memory : meri_default_memory();
    nslotmem = (size_t)(s.budget < SLOT_MAX - SLOT_SLACK ? s.budget + SLOT_SLACK
                                                         : SLOT_MAX);

    memset(r, 0, sizeof(*r));
    if (!p->ended || fid == LIMBA_NONE || p->fns[fid].nparams != 0) {
        r->status = MERI_BADENTRY;
        return;
    }
    globals = globals_new(&s);
    regs = reserve(REGS * sizeof(uint64_t));
    slotmem = reserve(nslotmem);
    frames = reserve(FRAMES * sizeof(frame));
    if (!globals || !regs || !slotmem || !frames) {
        r->status = MERI_TRAP;
        r->code = LIMBA_TRAP_NOMEM;
    } else {
        run(&s, &p->fns[fid], globals, regs, slotmem, nslotmem, frames, r);
    }
    if (regs)
        munmap(regs, REGS * sizeof(uint64_t));
    if (slotmem)
        munmap(slotmem, nslotmem);
    if (frames)
        munmap(frames, FRAMES * sizeof(frame));
    if (globals) {
        for (i = 0; i < p->m->nglobals; i++)
            free(globals[i]);
        free(globals);
    }
    for (i = 0; i < s.nstrs; i++)
        r->live_strings += s.strs[i]->rc != MERI_RC_IMMORTAL;
    r->live_refs = meri_big_alive(&s);
#ifdef MERI_PROFILE
    prof_dump();
#endif
    meri_heap_clear(&s.heap);
    meri_state_free_strs(&s);
    meri_big_free_all(&s);
}
