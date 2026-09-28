/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower.c - a module of the IR into bytecode, a function at a time.
 *
 * The window of a function: the registers the allocator gave
 * (0 .. nalloc - 1, the parameters first), one scratch register (nalloc),
 * then the outgoing arguments of its calls (from nalloc + 1). A call
 * copies its arguments there and the callee's window begins there, so
 * its parameters are its registers 0, 1, ...; the result comes back in
 * the first of them.
 *
 * A function that needs more (a register past 255, or outgoing arguments
 * past it) is emitted wide: five low registers, after the parameters,
 * are kept for it; an operand in a high register comes to one of the
 * first four with MOVEW, a result goes back from the first, the fifth is
 * the scratch register; its calls take the base of their arguments from
 * a second word (CALLW, CALLRTW, CALLIW).
 *
 * The copies of the arguments of a jump are done after the branch is
 * decided, on the way to their target: no block is added for a critical
 * edge. They are a parallel copy, ordered so that no register is written
 * before it is read; a cycle is broken with the scratch register.
 *
 * insieme_istruzioni.md § 4 says which operation becomes which
 * instruction.
 */
#include "vm/code.h"
#include "vm/lower.h"
#include "vm/vm.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t at;    /* the word of the JMP */
    uint32_t block; /* its target, or LIMBA_NONE for a word */
    uint32_t word;  /* the target word, when block is LIMBA_NONE */
} fixup;

typedef struct {
    const limba_module *m;
    const limba_func *f;
    meri_program *p;
    meri_fn *fn;
    uint32_t fid;
    meri_diag *d;
    bool failed;
    meri_alloc al;
    uint32_t scratch, outgoing, maxargs;
    bool wide;        /* registers past 255 (see the top) */
    uint32_t temp;    /* wide: the first of the four low temporaries */
    uint32_t pending; /* wide: the high register of the result, or 0 */
    uint32_t cur;     /* the position of the words being emitted */
    uint32_t *code, *pos, ncode, capcode, cappos;
    uint64_t *k;
    uint32_t nk, capk;
    uint32_t *label; /* of each block */
    uint32_t block;  /* the block being emitted */
    meri_strplan sp; /* where the strings stop living */
    uint32_t *chk;   /* of a ptr_live: the check fused with it, or NONE */
    uint8_t *gone;   /* a check fused into the ptr_live before it */
    fixup *fix;
    uint32_t nfix, capfix;
} L;

static void fail(L *l, const char *fmt, ...)
{
    va_list ap;
    size_t n;
    const char *name;
    int w;

    if (l->failed)
        return;
    l->failed = true;
    name = limba_str(l->m, l->f->name, &n);
    w = snprintf(l->d->msg, sizeof(l->d->msg), "function %.*s: ", (int)n, name);
    va_start(ap, fmt);
    if (w >= 0 && (size_t)w < sizeof(l->d->msg))
        vsnprintf(l->d->msg + w, sizeof(l->d->msg) - (size_t)w, fmt, ap);
    va_end(ap);
}

/* ---- emission ---- */

static bool grow(void **p, uint32_t *cap, uint32_t need, size_t size)
{
    uint32_t c;
    void *q;

    if (need <= *cap)
        return true;
    c = *cap ? *cap : 64;
    while (c < need) {
        if (c > UINT32_MAX / 2)
            return false;
        c *= 2;
    }
    q = realloc(*p, (size_t)c * size);
    if (!q)
        return false;
    *p = q;
    *cap = c;
    return true;
}

static uint32_t emit(L *l, uint32_t w)
{
    if (l->failed)
        return 0;
    if (l->ncode == MERI_SJ_MAX ||
        !grow((void **)&l->code, &l->capcode, l->ncode + 1, sizeof(uint32_t)) ||
        !grow((void **)&l->pos, &l->cappos, l->ncode + 1, sizeof(uint32_t))) {
        fail(l, "too much code, or out of memory");
        return 0;
    }
    l->code[l->ncode] = w;
    l->pos[l->ncode] = l->cur;
    return l->ncode++;
}

/* the index of constant v, added if new */
static uint32_t konst(L *l, uint64_t v)
{
    uint32_t i;

    for (i = 0; i < l->nk; i++)
        if (l->k[i] == v)
            return i;
    if (l->nk > 0xffff ||
        !grow((void **)&l->k, &l->capk, l->nk + 1, sizeof(uint64_t))) {
        fail(l, "more than 65536 constants");
        return 0;
    }
    l->k[l->nk] = v;
    return l->nk++;
}

/* two constants side by side, K[x] and K[x + 1] */
static uint32_t konst2(L *l, uint64_t a, uint64_t b)
{
    uint32_t i;

    for (i = 0; i + 1 < l->nk; i++)
        if (l->k[i] == a && l->k[i + 1] == b)
            return i;
    if (l->nk > 0xfffe ||
        !grow((void **)&l->k, &l->capk, l->nk + 2, sizeof(uint64_t))) {
        fail(l, "more than 65536 constants");
        return 0;
    }
    l->k[l->nk] = a;
    l->k[l->nk + 1] = b;
    l->nk += 2;
    return l->nk - 2;
}

/* a JMP to block b (or, with b = LIMBA_NONE, to a word fixed later) */
static uint32_t jump_to(L *l, uint32_t b)
{
    uint32_t at = emit(l, meri_sj(MERI_OP_JMP, 0));

    if (l->failed)
        return 0;
    if (!grow((void **)&l->fix, &l->capfix, l->nfix + 1, sizeof(fixup))) {
        fail(l, "out of memory");
        return 0;
    }
    l->fix[l->nfix] = (fixup){at, b, 0};
    return l->nfix++;
}

static unsigned reg(L *l, uint32_t v)
{
    uint16_t r = l->al.reg[v];

    if (r == MERI_NOREG) {
        fail(l, "value v%" PRIu32 " has no register", v);
        return 0;
    }
    return r;
}

static void move(L *l, unsigned dst, unsigned src)
{
    if (dst == src)
        return;
    if (dst > 255 || src > 255) {
        emit(l, meri_abc(MERI_OP_MOVEW, 0, 0, 0));
        emit(l, dst | (uint32_t)src << 16);
    } else {
        emit(l, meri_abc(MERI_OP_MOVE, dst, src, 0));
    }
}

/* a low register that holds value v: its own, or temporary t (0..3) */
static unsigned use(L *l, uint32_t v, unsigned t)
{
    unsigned r = reg(l, v);

    if (r <= 255)
        return r;
    move(l, l->temp + t, r);
    return l->temp + t;
}

/* a low register for the result v: its own, or the first temporary,
   copied to its own by done() */
static unsigned def(L *l, uint32_t v)
{
    unsigned r = reg(l, v);

    if (r <= 255)
        return r;
    l->pending = r;
    return l->temp;
}

static void done(L *l)
{
    if (l->pending) {
        move(l, l->pending, l->temp);
        l->pending = 0;
    }
}

static void load_const(L *l, unsigned dst, uint64_t v)
{
    int64_t s = (int64_t)v;

    if (s >= INT16_MIN && s <= INT16_MAX)
        emit(l, meri_abx(MERI_OP_LOADI, dst, (uint16_t)(int16_t)s));
    else
        emit(l, meri_abx(MERI_OP_LOADK, dst, konst(l, v)));
}

/* ---- the copies of a jump ---- */

typedef struct {
    unsigned dst, src;
} copy;

/* the arguments of a jump to their parameters: n pairs, as one parallel
   copy */
static void parallel_copy(L *l, copy *c, uint32_t n)
{
    uint32_t i, j;

    for (i = 0; i < n;) {
        if (c[i].dst == c[i].src) {
            c[i] = c[--n];
            continue;
        }
        i++;
    }
    while (n) {
        bool progress = false;
        for (i = 0; i < n; i++) {
            bool read = false;
            for (j = 0; j < n && !read; j++)
                read = j != i && c[j].src == c[i].dst;
            if (!read) {
                move(l, c[i].dst, c[i].src);
                c[i] = c[--n];
                progress = true;
                break;
            }
        }
        if (!progress) {
            /* every destination is still read: a cycle. Save one, and
               read it from the scratch register instead */
            unsigned d = c[0].dst;
            move(l, l->scratch, d);
            for (j = 0; j < n; j++)
                if (c[j].src == d)
                    c[j].src = l->scratch;
        }
    }
}

static bool is_str(const L *l, uint32_t v)
{
    return l->f->insts[v].type == LIMBA_T_STR;
}

static void release_list(L *l, const meri_list *d)
{
    uint32_t i;

    for (i = 0; i < d->n; i++)
        emit(l, meri_abc(MERI_OP_SRELEASE, use(l, l->sp.pool[d->first + i], 0),
                         0, 0));
}

/* the strings that stop living on edge i of the block being emitted */
static const meri_list *edge_deaths(const L *l, uint32_t i)
{
    return &l->sp.edge[l->sp.edge_at[l->block] + i];
}

/* the code of the jump whose (block, count, args) start at operand k of
   terminator t, edge i of its block: a reference for each str argument
   (the one of a value that dies on the edge moves with it), the releases
   of the other strings that die there, then the copies. The target block
   in *b */
static void edge_code(L *l, const limba_inst *t, uint32_t k, uint32_t i,
                      uint32_t *b)
{
    const uint32_t *o = l->f->operands + t->first + k;
    const limba_block *bl = &l->f->blocks[o[0]];
    const meri_list *d = edge_deaths(l, i);
    uint32_t n = o[1], j, q;
    uint8_t *moved = NULL;
    copy *c;

    *b = o[0];
    if (d->n && !(moved = calloc(d->n, 1))) {
        fail(l, "out of memory");
        return;
    }
    for (j = 0; j < n; j++) {
        uint32_t v = o[2 + j];
        bool take = true;
        if (!is_str(l, v))
            continue;
        for (q = 0; q < d->n && take; q++)
            if (l->sp.pool[d->first + q] == v && !moved[q])
                moved[q] = 1, take = false;
        if (take)
            emit(l, meri_abc(MERI_OP_SRETAIN, use(l, v, 0), 0, 0));
    }
    for (q = 0; q < d->n; q++)
        if (!moved[q])
            emit(l, meri_abc(MERI_OP_SRELEASE,
                             use(l, l->sp.pool[d->first + q], 0), 0, 0));
    free(moved);
    if (!n)
        return;
    c = malloc(n * sizeof(copy));
    if (!c) {
        fail(l, "out of memory");
        return;
    }
    for (j = 0; j < n; j++)
        c[j] = (copy){reg(l, bl->insts[j]), reg(l, o[2 + j])};
    parallel_copy(l, c, n);
    free(c);
}

/* true if edge i (arguments at operand k of t) needs code of its own */
static bool edge_has_code(const L *l, const limba_inst *t, uint32_t k,
                          uint32_t i)
{
    return l->f->operands[t->first + k + 1] != 0 || edge_deaths(l, i)->n;
}

/* ---- comparisons ---- */

/* the instruction that sets R[a] to the comparison cc of R[x], R[y] */
static void compare(L *l, unsigned a, unsigned cc, limba_id t, unsigned x,
                    unsigned y)
{
    static const struct {
        uint8_t op, swap;
    } ints[] = {
        [LIMBA_CC_EQ] = {MERI_OP_EQ, 0},   [LIMBA_CC_NE] = {MERI_OP_NE, 0},
        [LIMBA_CC_SLT] = {MERI_OP_LT, 0},  [LIMBA_CC_SLE] = {MERI_OP_LE, 0},
        [LIMBA_CC_SGT] = {MERI_OP_LT, 1},  [LIMBA_CC_SGE] = {MERI_OP_LE, 1},
        [LIMBA_CC_ULT] = {MERI_OP_LTU, 0}, [LIMBA_CC_ULE] = {MERI_OP_LEU, 0},
        [LIMBA_CC_UGT] = {MERI_OP_LTU, 1}, [LIMBA_CC_UGE] = {MERI_OP_LEU, 1},
    };

    if (!limba_cc_is_float(cc)) {
        unsigned b = ints[cc].swap ? y : x, c = ints[cc].swap ? x : y;
        emit(l, meri_abc(ints[cc].op, a, b, c));
        return;
    }
    if (t == LIMBA_T_F64) {
        switch (cc) {
        case LIMBA_CC_OEQ:
            emit(l, meri_abc(MERI_OP_FEQ, a, x, y));
            return;
        case LIMBA_CC_OLT:
            emit(l, meri_abc(MERI_OP_FLT, a, x, y));
            return;
        case LIMBA_CC_OLE:
            emit(l, meri_abc(MERI_OP_FLE, a, x, y));
            return;
        case LIMBA_CC_OGT:
            emit(l, meri_abc(MERI_OP_FLT, a, y, x));
            return;
        case LIMBA_CC_OGE:
            emit(l, meri_abc(MERI_OP_FLE, a, y, x));
            return;
        }
    }
    emit(l, meri_abc(MERI_OP_CMPX, a, x, y));
    emit(l, cc | (t == LIMBA_T_F32 ? 1u << 8 : 0));
}

/* a test and jump for cc of R[x], R[y]: *op, its operands and the k that
   jumps when the comparison holds; false if there is none */
static bool cond_jump(unsigned cc, limba_id t, unsigned x, unsigned y,
                      unsigned *op, unsigned *a, unsigned *b, unsigned *k)
{
    static const struct {
        uint8_t op, swap, k;
    } j[] = {
        [LIMBA_CC_EQ] = {MERI_OP_JEQ, 0, 1},
        [LIMBA_CC_NE] = {MERI_OP_JEQ, 0, 0},
        [LIMBA_CC_SLT] = {MERI_OP_JLT, 0, 1},
        [LIMBA_CC_SGE] = {MERI_OP_JLT, 0, 0},
        [LIMBA_CC_SLE] = {MERI_OP_JLE, 0, 1},
        [LIMBA_CC_SGT] = {MERI_OP_JLE, 0, 0},
        [LIMBA_CC_ULT] = {MERI_OP_JLTU, 0, 1},
        [LIMBA_CC_UGE] = {MERI_OP_JLTU, 0, 0},
        [LIMBA_CC_ULE] = {MERI_OP_JLEU, 0, 1},
        [LIMBA_CC_UGT] = {MERI_OP_JLEU, 0, 0},
        [LIMBA_CC_OEQ] = {MERI_OP_JFEQ, 0, 1},
        [LIMBA_CC_UNE] = {MERI_OP_JFEQ, 0, 0},
        [LIMBA_CC_OLT] = {MERI_OP_JFLT, 0, 1},
        [LIMBA_CC_FUGE] = {MERI_OP_JFLT, 0, 0},
        [LIMBA_CC_OLE] = {MERI_OP_JFLE, 0, 1},
        [LIMBA_CC_FUGT] = {MERI_OP_JFLE, 0, 0},
        [LIMBA_CC_OGT] = {MERI_OP_JFLT, 1, 1},
        [LIMBA_CC_FULE] = {MERI_OP_JFLT, 1, 0},
        [LIMBA_CC_OGE] = {MERI_OP_JFLE, 1, 1},
        [LIMBA_CC_FULT] = {MERI_OP_JFLE, 1, 0},
    };

    if (cc >= sizeof(j) / sizeof(j[0]) || !j[cc].op ||
        (limba_cc_is_float(cc) && t != LIMBA_T_F64))
        return false;
    *op = j[cc].op;
    *a = j[cc].swap ? y : x;
    *b = j[cc].swap ? x : y;
    *k = j[cc].k;
    return true;
}

/* a test of the condition of cbr t and a JMP taken when the condition is
   `when`, to block b (LIMBA_NONE: a word fixed later); the fixup index */
static uint32_t branch(L *l, const limba_inst *t, bool when, uint32_t b)
{
    uint32_t c = l->f->operands[t->first];
    unsigned op, x, y, k;

    if (l->al.fused[c]) {
        const limba_inst *ci = &l->f->insts[c];
        const uint32_t *o = l->f->operands + ci->first;
        limba_id ot = l->f->insts[o[0]].type;
        unsigned rx = use(l, o[0], 0), ry = use(l, o[1], 1);
        if (cond_jump(ci->cc, ot, rx, ry, &op, &x, &y, &k)) {
            emit(l, meri_abc(op, x, y, when ? k : !k));
        } else {
            compare(l, l->scratch, ci->cc, ot, rx, ry);
            emit(l, meri_abc(MERI_OP_TEST, l->scratch, when, 0));
        }
    } else {
        emit(l, meri_abc(MERI_OP_TEST, use(l, c, 0), when, 0));
    }
    return jump_to(l, b);
}

/* ---- the operations ---- */

/* the opcode of an integer operation of 64 or 32 bits, 0 for none */
static unsigned int_op(unsigned op, unsigned bits)
{
    static const uint8_t w64[LIMBA_OP_COUNT] = {
        [LIMBA_OP_ADD] = MERI_OP_ADD,     [LIMBA_OP_SUB] = MERI_OP_SUB,
        [LIMBA_OP_MUL] = MERI_OP_MUL,     [LIMBA_OP_ADDOV] = MERI_OP_ADDOV,
        [LIMBA_OP_SUBOV] = MERI_OP_SUBOV, [LIMBA_OP_MULOV] = MERI_OP_MULOV,
        [LIMBA_OP_SDIV] = MERI_OP_SDIV,   [LIMBA_OP_UDIV] = MERI_OP_UDIV,
        [LIMBA_OP_SREM] = MERI_OP_SREM,   [LIMBA_OP_UREM] = MERI_OP_UREM,
        [LIMBA_OP_SHL] = MERI_OP_SHL,     [LIMBA_OP_LSHR] = MERI_OP_LSHR,
        [LIMBA_OP_ASHR] = MERI_OP_ASHR,   [LIMBA_OP_NEG] = MERI_OP_NEG,
    };
    static const uint8_t w32[LIMBA_OP_COUNT] = {
        [LIMBA_OP_ADD] = MERI_OP_ADD32,     [LIMBA_OP_SUB] = MERI_OP_SUB32,
        [LIMBA_OP_MUL] = MERI_OP_MUL32,     [LIMBA_OP_ADDOV] = MERI_OP_ADDOV32,
        [LIMBA_OP_SUBOV] = MERI_OP_SUBOV32, [LIMBA_OP_MULOV] = MERI_OP_MULOV32,
        [LIMBA_OP_SDIV] = MERI_OP_SDIV32,   [LIMBA_OP_UDIV] = MERI_OP_UDIV32,
        [LIMBA_OP_SREM] = MERI_OP_SREM32,   [LIMBA_OP_UREM] = MERI_OP_UREM32,
        [LIMBA_OP_SHL] = MERI_OP_SHL32,     [LIMBA_OP_LSHR] = MERI_OP_LSHR32,
        [LIMBA_OP_ASHR] = MERI_OP_ASHR32,   [LIMBA_OP_NEG] = MERI_OP_NEG32,
    };

    return bits == 64 ? w64[op] : bits == 32 ? w32[op] : 0;
}

static void integer(L *l, const limba_inst *in, unsigned a, unsigned x,
                    unsigned y)
{
    unsigned bits = limba_type_bits(in->type);
    unsigned op;

    if (in->type == LIMBA_T_PTR)
        bits = 64;
    switch (in->op) {
    case LIMBA_OP_AND:
        emit(l, meri_abc(MERI_OP_AND, a, x, y));
        return;
    case LIMBA_OP_OR:
        emit(l, meri_abc(MERI_OP_OR, a, x, y));
        return;
    case LIMBA_OP_XOR:
        emit(l, meri_abc(MERI_OP_XOR, a, x, y));
        return;
    case LIMBA_OP_NOT:
        if (bits != 1) {
            emit(l, meri_abc(MERI_OP_NOT, a, x, 0));
            return;
        }
        break;
    }
    op = int_op(in->op, bits);
    if (op) {
        emit(l, meri_abc(op, a, x, y));
        return;
    }
    emit(l, meri_abc(MERI_OP_INTN, a, x, y));
    emit(l, in->op | bits << 8);
}

static void floating(L *l, const limba_inst *in, unsigned a, unsigned x,
                     unsigned y)
{
    bool f32 = in->type == LIMBA_T_F32;
    unsigned op;

    switch (in->op) {
    case LIMBA_OP_FADD:
        op = f32 ? MERI_OP_FADDF : MERI_OP_FADD;
        break;
    case LIMBA_OP_FSUB:
        op = f32 ? MERI_OP_FSUBF : MERI_OP_FSUB;
        break;
    case LIMBA_OP_FMUL:
        op = f32 ? MERI_OP_FMULF : MERI_OP_FMUL;
        break;
    case LIMBA_OP_FDIV:
        op = f32 ? MERI_OP_FDIVF : MERI_OP_FDIV;
        break;
    case LIMBA_OP_FNEG:
        op = f32 ? MERI_OP_FNEGF : MERI_OP_FNEG;
        break;
    case LIMBA_OP_FROUND:
        op = f32 ? MERI_OP_FROUNDF : MERI_OP_FROUND;
        break;
    case LIMBA_OP_FROUNDA:
        op = f32 ? MERI_OP_FROUNDAF : MERI_OP_FROUNDA;
        break;
    default:
        fail(l, "no float operation %s", limba_ops[in->op].text);
        return;
    }
    emit(l, meri_abc(op, a, x, y));
}

/* sign-extend (sext) or zero-extend from the low bits: 1, 8, 16, 32 */
static void extend(L *l, bool sign, unsigned bits, unsigned a, unsigned x)
{
    unsigned op;

    switch (bits) {
    case 1:
        op = sign ? MERI_OP_NEG : MERI_OP_ZEXT1;
        break;
    case 8:
        op = sign ? MERI_OP_SEXT8 : MERI_OP_ZEXT8;
        break;
    case 16:
        op = sign ? MERI_OP_SEXT16 : MERI_OP_ZEXT16;
        break;
    case 32:
        op = sign ? MERI_OP_SEXT32 : MERI_OP_ZEXT32;
        break;
    default:
        move(l, a, x);
        return;
    }
    emit(l, meri_abc(op, a, x, 0));
}

/* the canonical form of type t from the low bits of R[x]: sign-extended,
   and for i1 its low bit */
static void narrow(L *l, limba_id t, unsigned a, unsigned x)
{
    unsigned bits = limba_type_bits(t);

    if (bits == 1)
        emit(l, meri_abc(MERI_OP_ZEXT1, a, x, 0));
    else
        extend(l, true, bits, a, x);
}

static unsigned bits_of(limba_id t)
{
    return t == LIMBA_T_PTR ? 64 : limba_type_bits(t);
}

static void convert(L *l, const limba_inst *in, unsigned a, unsigned x)
{
    limba_id from = l->f->insts[l->f->operands[in->first]].type;
    limba_id to = in->type;

    switch (in->op) {
    case LIMBA_OP_TRUNC:
    case LIMBA_OP_PTRTOINT:
        narrow(l, to, a, x);
        return;
    case LIMBA_OP_ZEXT:
    case LIMBA_OP_INTTOPTR:
        /* i1 is 0 or 1 already */
        if (bits_of(from) == 1)
            move(l, a, x);
        else
            extend(l, false, bits_of(from), a, x);
        return;
    case LIMBA_OP_SEXT: /* canonical already, but i1 */
        if (bits_of(from) == 1)
            emit(l, meri_abc(MERI_OP_NEG, a, x, 0));
        else
            move(l, a, x);
        return;
    case LIMBA_OP_FPTRUNC:
        emit(l, meri_abc(MERI_OP_D2F, a, x, 0));
        return;
    case LIMBA_OP_FPEXT:
        emit(l, meri_abc(MERI_OP_F2D, a, x, 0));
        return;
    case LIMBA_OP_SITOFP:
        emit(l,
             meri_abc(to == LIMBA_T_F32 ? MERI_OP_I2F : MERI_OP_I2D, a, x, 0));
        return;
    case LIMBA_OP_UITOFP:
        if (bits_of(from) != 64 && bits_of(from) != 1) {
            extend(l, false, bits_of(from), a, x);
            x = a;
        }
        emit(l,
             meri_abc(to == LIMBA_T_F32 ? MERI_OP_U2F : MERI_OP_U2D, a, x, 0));
        return;
    case LIMBA_OP_BITCAST:
        if (from == LIMBA_T_I32)
            extend(l, false, 32, a, x); /* an f32 is zero-extended */
        else if (from == LIMBA_T_F32)
            extend(l, true, 32, a, x); /* an i32 is sign-extended */
        else
            move(l, a, x);
        return;
    default: /* fptosi, fptoui */
        emit(l, meri_abc(MERI_OP_CONVX, a, x, 0));
        emit(l, in->op | from << 8 | to << 16);
    }
}

static unsigned load_op(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
        return MERI_OP_LD1;
    case LIMBA_T_I8:
        return MERI_OP_LD8;
    case LIMBA_T_I16:
        return MERI_OP_LD16;
    case LIMBA_T_I32:
        return MERI_OP_LD32;
    case LIMBA_T_F32:
        return MERI_OP_LDF32;
    default:
        return MERI_OP_LD64;
    }
}

static unsigned store_op(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
    case LIMBA_T_I8:
        return MERI_OP_ST8;
    case LIMBA_T_I16:
        return MERI_OP_ST16;
    case LIMBA_T_I32:
    case LIMBA_T_F32:
        return MERI_OP_ST32;
    default:
        return MERI_OP_ST64;
    }
}

/* the arguments o[0..n) into the outgoing registers */
static void outgoing(L *l, const uint32_t *o, uint32_t n)
{
    uint32_t i;

    if (n > l->maxargs)
        l->maxargs = n;
    for (i = 0; i < n; i++)
        move(l, l->outgoing + i, reg(l, o[i]));
}

static void call(L *l, uint32_t id, const limba_inst *in)
{
    const uint32_t *o = l->f->operands + in->first;
    unsigned a = l->outgoing;

    if (l->maxargs < 1)
        l->maxargs = 1; /* the result */
    switch (in->op) {
    case LIMBA_OP_CALL:
        outgoing(l, o, in->nops);
        if (in->imm < 0 || in->imm > 0xffff)
            fail(l, "calls function %" PRId64 ", past 65535", in->imm);
        if (a > 255) { /* the base of the arguments in a second word */
            emit(l, meri_abx(MERI_OP_CALLW, 0, (uint32_t)in->imm));
            emit(l, a);
        } else {
            emit(l, meri_abx(MERI_OP_CALL, a, (uint32_t)in->imm));
        }
        break;
    case LIMBA_OP_CALLRT:
        outgoing(l, o, in->nops);
        if (a > 255) {
            emit(l, meri_abx(MERI_OP_CALLRTW, 0, (uint32_t)in->imm));
            emit(l, a);
        } else {
            emit(l, meri_abx(MERI_OP_CALLRT, a, (uint32_t)in->imm));
        }
        break;
    case LIMBA_OP_CALLIND: {
        unsigned c;
        outgoing(l, o + 1, in->nops - 1);
        c = use(l, o[0], 0);
        if (a > 255) /* the base split in A (low) and C (high) */
            emit(l, meri_abc(MERI_OP_CALLIW, a & 0xff, c, a >> 8));
        else
            emit(l, meri_abc(MERI_OP_CALLI, a, c, 0));
        emit(l, (uint32_t)in->imm);
        break;
    }
    default: /* call.ext: an error when it runs */
        outgoing(l, o, in->nops);
        emit(l, meri_abx(MERI_OP_CALLX, a, (uint32_t)in->imm & 0xffff));
    }
    if (meri_has_value(in))
        move(l, reg(l, id), a);
}

static void cbr(L *l, const limba_inst *t, uint32_t next)
{
    const uint32_t *o = l->f->operands + t->first;
    uint32_t tk = 1, ek = 3 + o[2], tb = o[1], eb = o[ek], i;
    bool tc = edge_has_code(l, t, tk, 0), ec = edge_has_code(l, t, ek, 1);

    if (!tc && !ec) {
        if (tb == next) {
            branch(l, t, false, eb);
        } else if (eb == next) {
            branch(l, t, true, tb);
        } else {
            branch(l, t, true, tb);
            jump_to(l, eb);
        }
        return;
    }
    /* to the else copies (or straight to else) when false; the then
       copies; then the else copies, if any */
    i = branch(l, t, false, ec ? LIMBA_NONE : eb);
    edge_code(l, t, tk, 0, &tb);
    if (ec || tb != next)
        jump_to(l, tb);
    if (ec) {
        if (!l->failed)
            l->fix[i].word = l->ncode;
        edge_code(l, t, ek, 1, &eb);
        if (eb != next)
            jump_to(l, eb);
    }
}

static void sw(L *l, const limba_inst *t)
{
    const uint32_t *o = l->f->operands + t->first;
    limba_id st = l->f->insts[o[0]].type;
    unsigned v = use(l, o[0], 0);
    uint32_t c, *stub = NULL;

    /* a case whose edge releases strings goes to a stub that does it */
    if (o[2] && !(stub = calloc(o[2], sizeof(uint32_t)))) {
        fail(l, "out of memory");
        return;
    }
    for (c = 0; c < o[2]; c++) {
        uint64_t cv = (uint64_t)o[4 + 3 * c] << 32 | o[3 + 3 * c];
        bool code = edge_deaths(l, 1 + c)->n != 0;
        load_const(l, l->scratch, meri_norm(cv, st));
        emit(l, meri_abc(MERI_OP_JEQ, v, l->scratch, 1));
        stub[c] = jump_to(l, code ? LIMBA_NONE : o[5 + 3 * c]);
    }
    release_list(l, edge_deaths(l, 0));
    jump_to(l, o[1]);
    for (c = 0; c < o[2]; c++)
        if (edge_deaths(l, 1 + c)->n) {
            if (!l->failed)
                l->fix[stub[c]].word = l->ncode;
            release_list(l, edge_deaths(l, 1 + c));
            jump_to(l, o[5 + 3 * c]);
        }
    free(stub);
}

static uint32_t string_index(L *l, limba_id s)
{
    meri_program *p = l->p;
    size_t n;
    const char *text;
    uint32_t i;

    for (i = 0; i < p->nstrs; i++)
        if (p->str_ids[i] == s)
            return i;
    if (p->nstrs > 0xffff) {
        fail(l, "more than 65536 string constants");
        return 0;
    }
    if (p->nstrs == p->capstrs) {
        uint32_t cap = p->capstrs ? 2 * p->capstrs : 64;
        meri_str **strs = realloc(p->strs, cap * sizeof(*strs));
        limba_id *ids = strs ? realloc(p->str_ids, cap * sizeof(*ids)) : NULL;
        if (strs)
            p->strs = strs;
        if (!strs || !ids) {
            fail(l, "out of memory");
            return 0;
        }
        p->str_ids = ids;
        p->capstrs = cap;
    }
    text = limba_str(l->m, s, &n);
    p->strs[p->nstrs] = meri_str_new(text, n);
    if (!p->strs[p->nstrs]) {
        fail(l, "out of memory");
        return 0;
    }
    p->str_ids[p->nstrs] = s;
    return p->nstrs++;
}

/* a runtime call of f64 -> f64 as an instruction of its own (FSQRT,
   FMATH): no copies to the outgoing registers; false if in is not one */
static bool math1(L *l, uint32_t id, const limba_inst *in)
{
    const uint32_t *o = l->f->operands + in->first;
    unsigned d, x;

    switch (in->imm) {
    case LIMBA_RT_MATH_SQRT:
    case LIMBA_RT_MATH_SIN:
    case LIMBA_RT_MATH_COS:
    case LIMBA_RT_MATH_TAN:
    case LIMBA_RT_MATH_ATAN:
    case LIMBA_RT_MATH_EXP:
    case LIMBA_RT_MATH_LN:
    case LIMBA_RT_MATH_TRUNC:
    case LIMBA_RT_MATH_FLOOR:
    case LIMBA_RT_MATH_CEIL:
        break;
    default:
        return false;
    }
    if (in->nops != 1 || !meri_has_value(in) || in->imm > 255)
        return false;
    x = use(l, o[0], 0);
    d = def(l, id);
    if (in->imm == LIMBA_RT_MATH_SQRT)
        emit(l, meri_abc(MERI_OP_FSQRT, d, x, 0));
    else
        emit(l, meri_abc(MERI_OP_FMATH, d, x, (unsigned)in->imm));
    return true;
}

/* the ptr_live whose only use is the check right after it: fused */
static bool find_chklive(L *l)
{
    const limba_func *f = l->f;
    uint32_t *uses = calloc((size_t)f->ninsts + 1, sizeof(uint32_t));
    uint32_t b, i, s, j;

    l->chk = malloc(((size_t)f->ninsts + 1) * sizeof(uint32_t));
    l->gone = calloc((size_t)f->ninsts + 1, 1);
    if (!uses || !l->chk || !l->gone) {
        free(uses);
        return false;
    }
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t ns = meri_value_spans(f, &f->insts[i], sp);
        l->chk[i] = LIMBA_NONE;
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++)
                uses[sp[s].o[j]]++;
    }
    for (b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        for (i = bl->nparams; i + 1 < bl->ninsts; i++) {
            uint32_t a = bl->insts[i], c = bl->insts[i + 1];
            const limba_inst *x = &f->insts[a], *y = &f->insts[c];
            if (x->op == LIMBA_OP_CALLRT && x->imm == LIMBA_RT_PTR_LIVE &&
                y->op == LIMBA_OP_CHECK && f->operands[y->first] == a &&
                uses[a] == 1) {
                l->chk[a] = c;
                l->gone[c] = 1;
            }
        }
    }
    free(uses);
    return true;
}

static void inst_body(L *l, uint32_t id, uint32_t next)
{
    const limba_func *f = l->f;
    const limba_inst *in = &f->insts[id];
    const uint32_t *o = f->operands + in->first;
    /* a call moves its result itself */
    bool calls = limba_ops[in->op].flags & LIMBA_OPF_CALL;
    unsigned a =
        meri_has_value(in) && !l->al.fused[id] && !calls ? def(l, id) : 0;
    uint32_t b;

    l->cur = limba_inst_pos(f, id);
    switch (limba_ops[in->op].format) {
    case LIMBA_F_ICONST:
        load_const(l, a, meri_norm((uint64_t)in->imm, in->type));
        return;
    case LIMBA_F_FCONST:
        load_const(l, a,
                   in->type == LIMBA_T_F32 ? (uint64_t)(uint32_t)in->imm
                                           : (uint64_t)in->imm);
        return;
    case LIMBA_F_SCONST:
        emit(l, meri_abx(MERI_OP_LOADS, a, string_index(l, (limba_id)in->imm)));
        return;
    case LIMBA_F_TYPED: /* null and undef: 0, the same on every run */
        load_const(l, a, 0);
        return;
    case LIMBA_F_UN:
        if (limba_type_is_float(in->type))
            floating(l, in, a, use(l, o[0], 0), 0);
        else
            integer(l, in, a, use(l, o[0], 0), 0);
        return;
    case LIMBA_F_BIN:
        if (limba_type_is_float(in->type))
            floating(l, in, a, use(l, o[0], 0), use(l, o[1], 1));
        else
            integer(l, in, a, use(l, o[0], 0), use(l, o[1], 1));
        return;
    case LIMBA_F_TERN: {
        /* the operands first: a MOVEW must not fall between the words */
        unsigned x = use(l, o[0], 0), y = use(l, o[1], 1), z = use(l, o[2], 2);
        if (in->op == LIMBA_OP_SELECT) {
            emit(l, meri_abc(MERI_OP_SELECT, a, x, y));
            emit(l, z);
            if (in->type == LIMBA_T_STR) /* a value of its own */
                emit(l, meri_abc(MERI_OP_SRETAIN, a, 0, 0));
        } else {
            emit(l,
                 meri_abc(in->type == LIMBA_T_F32 ? MERI_OP_FMAF : MERI_OP_FMA,
                          a, x, y));
            emit(l, z);
        }
        return;
    }
    case LIMBA_F_CMP:
        if (!l->al.fused[id])
            compare(l, a, in->cc, f->insts[o[0]].type, use(l, o[0], 0),
                    use(l, o[1], 1));
        return;
    case LIMBA_F_CONV:
        convert(l, in, a, use(l, o[0], 0));
        return;
    case LIMBA_F_LOAD:
        emit(l,
             meri_abc(in->type == LIMBA_T_STR ? MERI_OP_LDS : load_op(in->type),
                      a, use(l, o[0], 0), 0));
        return;
    case LIMBA_F_STORE: {
        limba_id vt = f->insts[o[0]].type;
        emit(l, meri_abc(vt == LIMBA_T_STR ? MERI_OP_STS : store_op(vt),
                         use(l, o[0], 0), use(l, o[1], 1), 0));
        return;
    }
    case LIMBA_F_SLOT:
    case LIMBA_F_GADDR:
    case LIMBA_F_FADDR:
        if (in->imm < 0 || in->imm > 0xffff) {
            fail(l, "%s %" PRId64 ", past 65535", limba_ops[in->op].text,
                 in->imm);
            return;
        }
        emit(l, meri_abx(in->op == LIMBA_OP_SLOT    ? MERI_OP_SLOT
                         : in->op == LIMBA_OP_GADDR ? MERI_OP_GADDR
                                                    : MERI_OP_FADDR,
                         a, (uint32_t)in->imm));
        return;
    case LIMBA_F_ADDR:
        emit(l, meri_abc(MERI_OP_ADDR, a, use(l, o[0], 0), use(l, o[1], 1)));
        emit(l, konst2(l, (uint64_t)in->imm, (uint64_t)in->imm2));
        return;
    case LIMBA_F_MEM3: {
        limba_id lt = f->insts[o[2]].type;
        unsigned len = use(l, o[2], 2);
        if (bits_of(lt) != 64) {
            if (bits_of(lt) == 1)
                move(l, l->scratch, len);
            else
                extend(l, false, bits_of(lt), l->scratch, len);
            len = l->scratch;
        }
        emit(l, meri_abc(in->op == LIMBA_OP_MEMCPY ? MERI_OP_MEMCPY
                                                   : MERI_OP_MEMSET,
                         use(l, o[0], 0), use(l, o[1], 1), len));
        return;
    }
    case LIMBA_F_RC:
        emit(l, meri_abc(MERI_OP_RC, use(l, o[0], 0), use(l, o[1], 1),
                         in->op == LIMBA_OP_RELEASE));
        emit(l, (uint32_t)in->imm);
        return;
    case LIMBA_F_CALL_RT:
        if (math1(l, id, in))
            return;
        if (l->chk && l->chk[id] != LIMBA_NONE) {
            /* ptr_live and its check: one instruction, where the check is */
            uint32_t c = l->chk[id];
            unsigned p0 = use(l, o[0], 0);
            l->cur = limba_inst_pos(f, c);
            emit(l, meri_abx(MERI_OP_CHKLIVE, p0,
                             konst(l, (uint64_t)f->insts[c].imm)));
            return;
        }
        call(l, id, in);
        return;
    case LIMBA_F_CALL:
    case LIMBA_F_CALL_IND:
    case LIMBA_F_CALL_EXT:
        call(l, id, in);
        return;
    case LIMBA_F_BR:
        edge_code(l, in, 0, 0, &b);
        if (b != next)
            jump_to(l, b);
        return;
    case LIMBA_F_CBR:
        cbr(l, in, next);
        return;
    case LIMBA_F_SWITCH:
        sw(l, in);
        return;
    case LIMBA_F_RET:
        release_list(l, &l->sp.at_ret[l->block]);
        if (l->fn->nrel_slots)
            emit(l, meri_abc(MERI_OP_RELSLOTS, 0, 0, 0));
        if (in->nops)
            emit(l, meri_abc(MERI_OP_RET, use(l, o[0], 0), 0, 0));
        else
            emit(l, meri_abc(MERI_OP_RET0, 0, 0, 0));
        return;
    case LIMBA_F_NONE:
        emit(l, meri_abc(MERI_OP_UNREACH, 0, 0, 0));
        return;
    case LIMBA_F_TRAP:
        emit(l, meri_abx(MERI_OP_TRAP, 0, konst(l, (uint64_t)in->imm)));
        return;
    case LIMBA_F_CHECK:
        if (l->gone && l->gone[id])
            return; /* done by CHKLIVE */
        emit(l, meri_abx(MERI_OP_CHECK, use(l, o[0], 0),
                         konst(l, (uint64_t)in->imm)));
        return;
    case LIMBA_F_PARAM:
        return;
    }
    fail(l, "operation %s not translated", limba_ops[in->op].text);
}

/* one instruction, and its result back to a high register if it has one */
static void inst(L *l, uint32_t id, uint32_t next)
{
    inst_body(l, id, next);
    done(l);
}

/* the slots of f in its frame, each aligned (8 bytes at least, 1 byte at
   least, as the reference interpreter gives them) */
static void layout_slots(L *l, meri_fn *fn)
{
    const limba_func *f = l->f;
    uint64_t off = 0, align = 16;
    uint32_t s;

    fn->nslots = f->nslots;
    fn->slot_off = calloc((size_t)f->nslots + 1, sizeof(uint64_t));
    if (!fn->slot_off) {
        fail(l, "out of memory");
        return;
    }
    for (s = 0; s < f->nslots; s++) {
        uint64_t a = f->slots[s].align < 8 ? 8 : f->slots[s].align;
        uint64_t size = f->slots[s].size ? f->slots[s].size : 1;
        if (a & (a - 1)) {
            fail(l, "slot %" PRIu32 " has an alignment of %" PRIu64, s, a);
            return;
        }
        /* the sizes are 32 bits, the offsets stay far below 2^63 */
        off = (off + a - 1) & ~(a - 1);
        fn->slot_off[s] = off;
        off += size;
        if (a > align)
            align = a;
    }
    fn->slot_size = (off + 15) & ~(uint64_t)15;
    fn->slot_align = align;
    fn->rel_slots = calloc((size_t)f->nslots + 1, sizeof(uint32_t));
    if (!fn->rel_slots) {
        fail(l, "out of memory");
        return;
    }
    for (s = 0; s < f->nslots; s++)
        if (f->slots[s].type != LIMBA_NONE && l->p->holds_str[f->slots[s].type])
            fn->rel_slots[fn->nrel_slots++] = s;
}

/* the most arguments a call of f passes (1 at least if f calls: the
   result comes back in the first) */
static uint32_t max_call_args(const limba_func *f)
{
    uint32_t i, n = 0;

    for (i = 0; i < f->ninsts; i++) {
        const limba_inst *in = &f->insts[i];
        uint32_t k;
        if (!(limba_ops[in->op].flags & LIMBA_OPF_CALL))
            continue;
        k = in->nops - (in->op == LIMBA_OP_CALLIND);
        if (k < 1)
            k = 1;
        if (k > n)
            n = k;
    }
    return n;
}

/* the registers of f: narrow if everything fits in 256, wide otherwise
   (see the top); false, with a message, if even that is not enough */
static bool registers(L *l, uint32_t nparams)
{
    const limba_func *f = l->f;
    uint32_t nargs = max_call_args(f);

    if (meri_alloc_regs(f, 255, 255, 0, &l->al)) {
        l->scratch = l->al.nregs > nparams ? l->al.nregs : nparams;
        l->outgoing = l->scratch + 1;
        if (l->outgoing + nargs <= 256)
            return true;
    }
    meri_alloc_free(&l->al);
    if (nparams > 250) {
        fail(l, "more than 250 parameters and 255 registers");
        return false;
    }
    l->wide = true;
    l->temp = nparams;
    l->scratch = l->temp + 4;
    if (!meri_alloc_regs(f, MERI_NOREG, l->temp, 5, &l->al)) {
        fail(l, "needs more than 65535 registers, or memory");
        return false;
    }
    l->outgoing = l->al.nregs > l->scratch + 1 ? l->al.nregs : l->scratch + 1;
    if (l->outgoing + nargs > 65535) {
        fail(l, "needs more than 65535 registers with its calls");
        return false;
    }
    return true;
}

static void function(meri_program *p, uint32_t fid, meri_diag *d)
{
    const limba_func *f = &p->m->funcs[fid];
    meri_fn *fn = &p->fns[fid];
    L l = {.m = p->m, .f = f, .p = p, .fid = fid, .d = d, .fn = fn};
    bool body = f->nblocks > 0; /* declared only: calling it is unreachable */
    uint32_t b, i;

    fn->type = f->type;
    fn->nparams = p->m->types[f->type].count;
    if (body && !registers(&l, fn->nparams)) {
        p->failed = true;
        meri_alloc_free(&l.al);
        return;
    }
    if (body && !find_chklive(&l)) {
        fail(&l, "out of memory");
        p->failed = true;
        meri_alloc_free(&l.al);
        return;
    }
    if (body && !meri_str_plan(f, &l.sp)) {
        fail(&l, "out of memory");
        p->failed = true;
        meri_alloc_free(&l.al);
        return;
    }
    if (!body) {
        l.scratch = fn->nparams;
        l.outgoing = l.scratch + 1;
    }
    if (body) {
        /* the slots first: the returns release the typed ones */
        layout_slots(&l, fn);
        l.label = calloc(f->nblocks, sizeof(uint32_t));
        if (!l.label)
            fail(&l, "out of memory");
        /* the parameters are the callee's own: a reference each, taken
           before block 0, which a jump may enter again */
        for (i = 0; i < f->blocks[0].nparams; i++)
            if (is_str(&l, f->blocks[0].insts[i]))
                emit(&l, meri_abc(MERI_OP_SRETAIN,
                                  use(&l, f->blocks[0].insts[i], 0), 0, 0));
        for (b = 0; b < f->nblocks && !l.failed; b++) {
            const limba_block *bl = &f->blocks[b];
            uint32_t next = b + 1 < f->nblocks ? b + 1 : LIMBA_NONE;
            l.block = b;
            l.label[b] = l.ncode;
            release_list(&l, &l.sp.start[b]);
            for (i = bl->nparams; i < bl->ninsts; i++) {
                uint32_t id = bl->insts[i];
                inst(&l, id, next);
                if (i + 1 < bl->ninsts)
                    release_list(&l, &l.sp.after[id]);
            }
        }
        for (i = 0; i < l.nfix && !l.failed; i++) {
            uint32_t to = l.fix[i].block == LIMBA_NONE
                              ? l.fix[i].word
                              : l.label[l.fix[i].block];
            int64_t off = (int64_t)to - (int64_t)l.fix[i].at - 1;
            if (off < MERI_SJ_MIN || off > MERI_SJ_MAX)
                fail(&l, "a jump too far");
            l.code[l.fix[i].at] = meri_sj(MERI_OP_JMP, (int32_t)off);
        }
    } else {
        l.cur = 0;
        emit(&l, meri_abc(MERI_OP_UNREACH, 0, 0, 0));
    }
    fn->nregs = l.outgoing + l.maxargs;
    fn->code = l.code;
    fn->ncode = l.ncode;
    fn->pos = l.pos;
    fn->k = l.k;
    fn->nk = l.nk;
    free(l.label);
    free(l.fix);
    meri_alloc_free(&l.al);
    meri_strplan_free(&l.sp);
    free(l.chk);
    free(l.gone);
    if (l.failed)
        p->failed = true;
}

meri_program *meri_compile(const limba_module *m, meri_diag *d)
{
    meri_program *p = calloc(1, sizeof(*p));
    uint32_t i;

    d->msg[0] = 0;
    if (!p || m->nfuncs > 0x10000 || m->nglobals > 0x10000) {
        snprintf(d->msg, sizeof(d->msg), "%s",
                 p ? "more than 65536 functions or globals" : "out of memory");
        free(p);
        return NULL;
    }
    p->m = m;
    p->nfns = m->nfuncs;
    p->fns = calloc((size_t)m->nfuncs + 1, sizeof(meri_fn));
    if (!p->fns) {
        snprintf(d->msg, sizeof(d->msg), "out of memory");
        free(p);
        return NULL;
    }
    p->holds_str = calloc((size_t)m->ntypes + 1, 1);
    if (!p->holds_str) {
        snprintf(d->msg, sizeof(d->msg), "out of memory");
        meri_program_free(p);
        return NULL;
    }
    for (i = 0; i < m->ntypes; i++)
        p->holds_str[i] = limba_type_holds_str(m, i);
    for (i = 0; i < m->nfuncs && !p->failed; i++)
        function(p, i, d);
    if (p->failed) {
        meri_program_free(p);
        return NULL;
    }
    return p;
}

void meri_program_free(meri_program *p)
{
    uint32_t i;

    if (!p)
        return;
    for (i = 0; i < p->nfns; i++) {
        free(p->fns[i].code);
        free(p->fns[i].pos);
        free(p->fns[i].k);
        free(p->fns[i].slot_off);
        free(p->fns[i].rel_slots);
    }
    for (i = 0; i < p->nstrs; i++)
        free(p->strs[i]);
    free(p->fns);
    free(p->strs);
    free(p->str_ids);
    free(p->holds_str);
    free(p);
}
