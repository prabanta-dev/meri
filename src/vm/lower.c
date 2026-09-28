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
    meri_fusion fu;  /* instructions absorbed by a later one */
    uint32_t *rlo, *rhi, *ridx; /* of a check: a range check fused in */
    uint32_t *ldx;  /* of a load or store: the addr fused in, or NONE */
    uint32_t *fold; /* of an addr: the sub i, c folded in, or NONE */
    uint8_t *gone;  /* a check fused into the ptr_live before it */
    uint32_t *nz;   /* of a check: x, when it checks ne x, 0 (then check x) */
    uint32_t *tz;   /* of a comparison eq or ne x, 0 fused into its cbr: x */
    const uint32_t *order; /* the blocks as emitted */
    uint32_t *at_of;       /* of each block: its index in order */
    uint32_t at;           /* the index of the block being emitted */
    uint8_t *skip;         /* blocks emitted with the one before them */
    uint32_t *npred;       /* of each block: the jumps to it */
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

/* true if edge i (arguments at operand k of t, of block b) needs code of
   its own: strings that die on it, or a copy between two registers. A
   str argument needs a reference only if it is still alive after the
   jump, where the parameter begins: then their registers differ */
static bool edge_code_of(const L *l, uint32_t b, const limba_inst *t,
                         uint32_t k, uint32_t i)
{
    const uint32_t *o = l->f->operands + t->first + k;
    const limba_block *bl = &l->f->blocks[o[0]];
    uint32_t j;

    if (l->sp.edge[l->sp.edge_at[b] + i].n)
        return true;
    for (j = 0; j < o[1]; j++)
        if (l->al.reg[bl->insts[j]] != l->al.reg[o[2 + j]])
            return true;
    return false;
}

static bool edge_has_code(const L *l, const limba_inst *t, uint32_t k,
                          uint32_t i)
{
    return edge_code_of(l, l->block, t, k, i);
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

    if (l->al.fused[c] && l->tz && l->tz[c] != LIMBA_NONE) {
        /* eq or ne x, 0: TEST jumps when (x != 0) is its B */
        bool ne = l->f->insts[c].cc == LIMBA_CC_NE;
        emit(l,
             meri_abc(MERI_OP_TEST, use(l, l->tz[c], 0), ne ? when : !when, 0));
    } else if (l->al.fused[c]) {
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

/* a call that copies its arguments itself (CALLN, CALLRTN): the words
   of the registers after the first; false if an argument or the base is
   past 255 (then the arguments are moved one by one) */
static bool call_n(L *l, unsigned op, unsigned a, uint32_t bx,
                   const uint32_t *o, uint32_t n)
{
    uint32_t i, word, k;

    if (a > 255 || n > 255)
        return false;
    for (i = 0; i < n; i++)
        if (reg(l, o[i]) > 255)
            return false;
    if (n > l->maxargs)
        l->maxargs = n;
    emit(l, meri_abx(op, a, bx));
    word = n;
    for (i = 0; i < n; i++) {
        k = i + 1;
        word |= (uint32_t)reg(l, o[i]) << (8 * (k % 4));
        if (k % 4 == 3) {
            emit(l, word);
            word = 0;
        }
    }
    if ((n + 1) % 4 != 0)
        emit(l, word);
    return true;
}

static void call(L *l, uint32_t id, const limba_inst *in)
{
    const uint32_t *o = l->f->operands + in->first;
    unsigned a = l->outgoing;

    if (l->maxargs < 1)
        l->maxargs = 1; /* the result */
    switch (in->op) {
    case LIMBA_OP_CALL:
        if (in->imm < 0 || in->imm > 0xffff)
            fail(l, "calls function %" PRId64 ", past 65535", in->imm);
        if (in->nops &&
            call_n(l, MERI_OP_CALLN, a, (uint32_t)in->imm, o, in->nops))
            break;
        outgoing(l, o, in->nops);
        if (a > 255) { /* the base of the arguments in a second word */
            emit(l, meri_abx(MERI_OP_CALLW, 0, (uint32_t)in->imm));
            emit(l, a);
        } else {
            emit(l, meri_abx(MERI_OP_CALL, a, (uint32_t)in->imm));
        }
        break;
    case LIMBA_OP_CALLRT:
        if (in->nops &&
            call_n(l, MERI_OP_CALLRTN, a, (uint32_t)in->imm, o, in->nops))
            break;
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

/* the first block after the one being emitted that is emitted itself */
static uint32_t next_block(const L *l)
{
    uint32_t k;

    for (k = l->at + 1; k < l->f->nblocks; k++)
        if (!l->skip[l->order[k]])
            return l->order[k];
    return LIMBA_NONE;
}

/* the end of a loop, as LOOP or LOOP32: cbr t on eq or ne x, y, where
   the way on while x != y is a block of its own, not yet emitted, that
   only this jump enters, whose code is nothing but x2 = add x, s and a
   jump with no code to a block h (x2 in the register of x). Emitted: LOOP x, y,
   s and a JMP to h; then the other way, if its edge has code, and a jump to its
   block if it does not follow. The block of the add is skipped */
static bool loop_end(L *l, const limba_inst *t)
{
    const limba_func *f = l->f;
    const uint32_t *o = f->operands + t->first;
    uint32_t c = o[0], ek = 3 + o[2], tb = o[1], eb = o[ek];
    uint32_t go, stay, gk, gi, xk, xi, add = LIMBA_NONE, br = LIMBA_NONE, x, y,
                                       sv, h, nx, i, n = 0;
    const limba_inst *ci = &f->insts[c], *ai, *bi;
    const uint32_t *oc = f->operands + ci->first, *oa;
    const limba_block *gb;
    unsigned bits, op;

    if (l->wide || !l->al.fused[c] || (l->tz && l->tz[c] != LIMBA_NONE) ||
        ci->op != LIMBA_OP_ICMP ||
        (ci->cc != LIMBA_CC_EQ && ci->cc != LIMBA_CC_NE))
        return false;
    /* go: the way on (x != y), stay: the other */
    if (ci->cc == LIMBA_CC_EQ)
        go = eb, gk = ek, gi = 1, stay = tb, xk = 1, xi = 0;
    else
        go = tb, gk = 1, gi = 0, stay = eb, xk = ek, xi = 1;
    gb = &f->blocks[go];
    if (go == stay || go == l->block || l->npred[go] != 1 ||
        l->at_of[go] <= l->at || gb->nparams ||
        edge_code_of(l, l->block, t, gk, gi) || l->sp.start[go].n)
        return false;
    /* its instructions but the constants loaded before block 0 and those
       no one needs */
    for (i = 0; i < gb->ninsts; i++) {
        uint32_t v = gb->insts[i];
        if ((l->fu.hoist && l->fu.hoist[v]) || l->fu.absorbed[v])
            continue;
        if (n == 0)
            add = v;
        else
            br = v;
        n++;
    }
    if (n != 2)
        return false;
    ai = &f->insts[add], bi = &f->insts[br];
    oa = f->operands + ai->first;
    if (ai->op != LIMBA_OP_ADD || bi->op != LIMBA_OP_BR || l->sp.after[add].n ||
        edge_code_of(l, go, bi, 0, 0))
        return false;
    bits = bits_of(ai->type);
    if (bits != 64 && bits != 32)
        return false;
    op = bits == 64 ? MERI_OP_LOOP : MERI_OP_LOOP32;
    if (oa[0] == oc[0] || oa[0] == oc[1])
        x = oa[0], sv = oa[1];
    else if (oa[1] == oc[0] || oa[1] == oc[1])
        x = oa[1], sv = oa[0];
    else
        return false;
    y = x == oc[0] ? oc[1] : oc[0];
    if (reg(l, add) != reg(l, x) || f->insts[x].type != ai->type)
        return false;
    h = f->operands[bi->first];
    l->skip[go] = 1;
    l->cur = limba_inst_pos(f, add);
    emit(l, meri_abc(op, reg(l, x), reg(l, y), reg(l, sv)));
    jump_to(l, h);
    l->cur = limba_inst_pos(f, c);
    edge_code(l, t, xk, xi, &stay);
    nx = next_block(l);
    if (stay != nx)
        jump_to(l, stay);
    return true;
}

static void cbr(L *l, const limba_inst *t, uint32_t next)
{
    const uint32_t *o = l->f->operands + t->first;
    uint32_t tk = 1, ek = 3 + o[2], tb = o[1], eb = o[ek], i;
    bool tc, ec;

    if (loop_end(l, t))
        return;
    tc = edge_has_code(l, t, tk, 0), ec = edge_has_code(l, t, ek, 1);
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

/* a pointer into the bytes of a str (meri_str_of_ptr) keeps the str
   alive where it is used, so it may go only where that use is seen: the
   base of an addr, the address of a load or a store, memcpy, memset, the
   argument of a call. Anywhere else (a jump, a store of the pointer, a
   select, a return) the str could be freed while the pointer lives: the
   function is refused */
static bool str_ptrs_stay(L *l)
{
    const limba_func *f = l->f;
    uint32_t i, s, j;

    for (i = 0; i < f->ninsts; i++) {
        const limba_inst *in = &f->insts[i];
        const uint32_t *o = f->operands + in->first;
        meri_span sp[3];
        uint32_t ns = meri_value_spans(f, in, sp);
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++) {
                uint32_t u = sp[s].o[j];
                bool ok;
                if (meri_str_of_ptr(f, u) == LIMBA_NONE)
                    continue;
                switch (in->op) {
                case LIMBA_OP_ADDR:
                    ok = &sp[s].o[j] == o;
                    break;
                case LIMBA_OP_LOAD:
                case LIMBA_OP_LOADINV:
                case LIMBA_OP_MEMCPY:
                case LIMBA_OP_MEMSET:
                    ok = true;
                    break;
                case LIMBA_OP_STORE:
                    ok = &sp[s].o[j] == o + 1;
                    break;
                default:
                    ok = (limba_ops[in->op].flags & LIMBA_OPF_CALL) != 0;
                }
                if (!ok) {
                    fail(l,
                         "v%" PRIu32 " points into a str and goes to %s: "
                         "not supported",
                         u, limba_ops[in->op].text);
                    return false;
                }
            }
    }
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

/* ---- fusions: sequences that become one instruction ---- */

/* load or load.inv: for an engine the same (load.inv only tells an
   optimisation that the memory does not change while its block lives) */
static bool is_load(const limba_inst *in)
{
    return in->op == LIMBA_OP_LOAD || in->op == LIMBA_OP_LOADINV;
}

static bool is_const(const limba_func *f, uint32_t v)
{
    return f->insts[v].op == LIMBA_OP_ICONST;
}

/* a constant that can be loaded once, before block 0: not a str (those
   are counted, and their releases follow their places) */
static bool hoistable(const limba_func *f, uint32_t v)
{
    const limba_inst *in = &f->insts[v];
    unsigned fmt = limba_ops[in->op].format;

    return (fmt == LIMBA_F_ICONST || fmt == LIMBA_F_FCONST ||
            fmt == LIMBA_F_TYPED) &&
           in->type != LIMBA_T_STR;
}

/* the blocks that lie on a cycle of the CFG (in a strongly connected
   component with more than one block, or with an edge to itself), and
   the component of each block (comp may be NULL): Tarjan's algorithm,
   without recursion. false when memory is exhausted */
static bool loop_blocks(const limba_func *f, uint8_t *inloop, uint32_t *comp)
{
    uint32_t nb = f->nblocks, top = 0, ntop = 0, counter = 0, ncomp = 0, b, k;
    uint32_t *index = malloc((nb + 1) * sizeof(uint32_t));
    uint32_t *low = malloc((nb + 1) * sizeof(uint32_t));
    uint32_t *stack = malloc((nb + 1) * sizeof(uint32_t));
    uint32_t *call = malloc((nb + 1) * sizeof(uint32_t)); /* dfs: block */
    uint32_t *next = malloc((nb + 1) * sizeof(uint32_t)); /* dfs: succ */
    uint8_t *on = calloc(nb + 1, 1);

    if (!index || !low || !stack || !call || !next || !on) {
        free(index), free(low), free(stack), free(call), free(next), free(on);
        return false;
    }
    for (b = 0; b < nb; b++)
        index[b] = UINT32_MAX;
    for (b = 0; b < nb; b++) {
        if (index[b] != UINT32_MAX)
            continue;
        call[0] = b, next[0] = 0, ntop = 1;
        index[b] = low[b] = counter++;
        stack[top++] = b, on[b] = 1;
        while (ntop) {
            uint32_t v = call[ntop - 1];
            if (next[ntop - 1] < meri_nsuccs(f, v)) {
                uint32_t w = meri_succ(f, v, next[ntop - 1]++);
                if (w == v)
                    inloop[v] = 1; /* an edge to itself */
                if (index[w] == UINT32_MAX) {
                    index[w] = low[w] = counter++;
                    stack[top++] = w, on[w] = 1;
                    call[ntop] = w, next[ntop] = 0, ntop++;
                } else if (on[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
                continue;
            }
            if (low[v] == index[v]) { /* the root of a component */
                uint32_t size = 0, first = top;
                do
                    size++, first--;
                while (stack[first] != v);
                for (k = first; k < top; k++) {
                    on[stack[k]] = 0;
                    if (size > 1)
                        inloop[stack[k]] = 1;
                    if (comp)
                        comp[stack[k]] = ncomp;
                }
                ncomp++;
                top = first;
            }
            ntop--;
            if (ntop && low[v] < low[call[ntop - 1]])
                low[call[ntop - 1]] = low[v];
        }
    }
    free(index), free(low), free(stack), free(call), free(next), free(on);
    return true;
}

/* a conversion that changes no bit of a canonical value, emitted as a
   copy: its register can be the one of its source */
static bool pure_copy(const limba_func *f, const limba_inst *in)
{
    limba_id from = f->insts[f->operands[in->first]].type, to = in->type;
    unsigned fb = from == LIMBA_T_PTR ? 64 : limba_type_bits(from);
    unsigned tb = to == LIMBA_T_PTR ? 64 : limba_type_bits(to);

    switch (in->op) {
    case LIMBA_OP_SEXT:
        return fb != 1;
    case LIMBA_OP_ZEXT:
    case LIMBA_OP_INTTOPTR:
        return fb == 1 || fb == 64;
    case LIMBA_OP_PTRTOINT:
        return tb == 64;
    case LIMBA_OP_BITCAST:
        return from != LIMBA_T_I32 && from != LIMBA_T_F32;
    default:
        return false;
    }
}

static int64_t const_of(const limba_func *f, uint32_t v)
{
    return (int64_t)meri_norm((uint64_t)f->insts[v].imm, f->insts[v].type);
}

/* a comparison a <= b of signed integers, as (*lo, *hi): sle a, b or
   sge b, a; false for any other */
static bool less_eq(const limba_func *f, const limba_inst *x, uint32_t *lo,
                    uint32_t *hi)
{
    const uint32_t *o = f->operands + x->first;

    if (x->op != LIMBA_OP_ICMP)
        return false;
    if (x->cc == LIMBA_CC_SLE) {
        *lo = o[0], *hi = o[1];
        return true;
    }
    if (x->cc == LIMBA_CC_SGE) {
        *lo = o[1], *hi = o[0];
        return true;
    }
    return false;
}

/* the range check of Luxia, lo <= i and i <= hi then check (as sge i, lo
   and sle i, hi, or any order): two comparisons, and, check, each value
   used once, one after the other. *lo, *i, *hi */
static bool range_check(const limba_func *f, const uint32_t *uses,
                        const uint32_t *w, uint32_t *lo, uint32_t *i,
                        uint32_t *hi)
{
    const limba_inst *z = &f->insts[w[2]], *c = &f->insts[w[3]];
    const uint32_t *oz = f->operands + z->first;
    uint32_t a1, b1, a2, b2;

    if (!less_eq(f, &f->insts[w[0]], &a1, &b1) ||
        !less_eq(f, &f->insts[w[1]], &a2, &b2) || z->op != LIMBA_OP_AND ||
        c->op != LIMBA_OP_CHECK || f->operands[c->first] != w[2] ||
        uses[w[0]] != 1 || uses[w[1]] != 1 || uses[w[2]] != 1 ||
        !((oz[0] == w[0] && oz[1] == w[1]) || (oz[0] == w[1] && oz[1] == w[0])))
        return false;
    if (b1 == a2) { /* a1 <= i, i <= b2 */
        *lo = a1, *i = b1, *hi = b2;
        return true;
    }
    if (b2 == a1) { /* a2 <= i, i <= b1 */
        *lo = a2, *i = b2, *hi = b1;
        return true;
    }
    return false;
}

/* the constant 0 of an integer or pointer type (iconst 0, null) */
static bool is_zero(const limba_func *f, uint32_t v)
{
    return (is_const(f, v) && const_of(f, v) == 0) ||
           f->insts[v].op == LIMBA_OP_NULLV;
}

/* ne x, 0 used once, by the check right after it: *x */
static bool nonzero_check(const limba_func *f, const uint32_t *uses,
                          const uint32_t *w, uint32_t *x)
{
    const limba_inst *z = &f->insts[w[0]], *c = &f->insts[w[1]];
    const uint32_t *oz = f->operands + z->first;

    if (z->op != LIMBA_OP_ICMP || z->cc != LIMBA_CC_NE || uses[w[0]] != 1 ||
        c->op != LIMBA_OP_CHECK || f->operands[c->first] != w[0])
        return false;
    if (is_zero(f, oz[1]))
        *x = oz[0];
    else if (is_zero(f, oz[0]))
        *x = oz[1];
    else
        return false;
    return true;
}

/* addr a whose index (after a folded sub i, c) is a constant: its
   displacement from the base, modulo 2^64, in *off; false otherwise */
static bool addr_const(const L *l, uint32_t a, uint64_t *off)
{
    const limba_func *f = l->f;
    const limba_inst *ai = &f->insts[a];
    const uint32_t *oa = f->operands + ai->first;
    uint64_t scale = (uint64_t)ai->imm, disp = (uint64_t)ai->imm2;
    uint32_t idx = oa[1];

    if (l->fold[a] != LIMBA_NONE) {
        const uint32_t *ot = f->operands + f->insts[l->fold[a]].first;
        idx = ot[0];
        disp -= (uint64_t)const_of(f, ot[1]) * scale;
    }
    if (!is_const(f, idx))
        return false;
    *off = (uint64_t)const_of(f, idx) * scale + disp;
    return true;
}

/* addr a as base + a displacement of 0..255 (ld, st, addi) */
static bool addr_small(const L *l, uint32_t a)
{
    uint64_t off;

    return addr_const(l, a, &off) && off <= 255;
}

static bool absorb(L *l, uint32_t v, uint32_t anchor)
{
    l->fu.absorbed[v] = 1;
    l->fu.anchor[v] = anchor;
    return true;
}

/* find the fusions of f before its registers are given: range checks
   (CHKR, CHKRK), addr into the load or store right after it (LDX, STX),
   sub i, c into the addr right after it; constants no longer needed */
static bool find_fusions(L *l)
{
    const limba_func *f = l->f;
    size_t n = (size_t)f->ninsts + 1;
    uint32_t *uses = calloc(n, sizeof(uint32_t)), *need = NULL, *seq = NULL;
    uint32_t b, i, s, j, ns, k2;
    uint8_t *inloop = NULL;
    bool hoist;

    l->fu.absorbed = calloc(n, 1);
    l->fu.anchor = malloc(n * sizeof(uint32_t));
    l->rlo = malloc(n * sizeof(uint32_t));
    l->rhi = malloc(n * sizeof(uint32_t));
    l->ridx = malloc(n * sizeof(uint32_t));
    l->ldx = malloc(n * sizeof(uint32_t));
    l->fold = malloc(n * sizeof(uint32_t));
    l->nz = malloc(n * sizeof(uint32_t));
    l->tz = malloc(n * sizeof(uint32_t));
    l->fu.hoist = calloc(n, 1);
    need = calloc(n, sizeof(uint32_t));
    seq = malloc(n * sizeof(uint32_t));
    if (!uses || !need || !seq || !l->fu.absorbed || !l->fu.anchor || !l->rlo ||
        !l->rhi || !l->ridx || !l->ldx || !l->fold || !l->nz || !l->tz ||
        !l->fu.hoist) {
        free(uses);
        free(need);
        free(seq);
        return false;
    }
    /* the constants go before block 0, which no jump enters again (the
       verifier of Limba refuses a branch to the entry block), but only
       those used on a cycle: a function without loops, called often,
       would load them all at every call */
    hoist = true;
    l->fu.alias = malloc(n * sizeof(uint32_t));
    inloop = calloc((size_t)f->nblocks + 1, 1);
    if (!l->fu.alias || !inloop || !loop_blocks(f, inloop, NULL)) {
        free(uses), free(need), free(seq), free(inloop);
        return false;
    }
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t ns = meri_value_spans(f, &f->insts[i], sp);
        l->fu.anchor[i] = l->rlo[i] = l->rhi[i] = l->ridx[i] = LIMBA_NONE;
        l->ldx[i] = l->fold[i] = l->nz[i] = l->tz[i] = LIMBA_NONE;
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++)
                uses[sp[s].o[j]]++;
    }
    for (b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        /* the instructions as emitted: without the constants loaded
           before block 0, which no longer stand between the others */
        /* the operands of fused instructions are read at their anchor,
           so the constants between them do not matter */
        for (ns = 0, i = bl->nparams; i < bl->ninsts; i++)
            if (!hoistable(f, bl->insts[i]))
                seq[ns++] = bl->insts[i];
        for (i = 0; i < ns; i++) {
            const uint32_t *w = seq + i;
            uint32_t lo, x, hi;
            if (i + 3 < ns && range_check(f, uses, w, &lo, &x, &hi)) {
                l->rlo[w[3]] = lo, l->ridx[w[3]] = x, l->rhi[w[3]] = hi;
                absorb(l, w[0], w[3]);
                absorb(l, w[1], w[3]);
                absorb(l, w[2], w[3]);
            }
        }
        /* check (ne x, 0): check x */
        for (i = 0; i + 1 < ns; i++) {
            uint32_t x;
            if (!l->fu.absorbed[seq[i]] && !l->fu.absorbed[seq[i + 1]] &&
                nonzero_check(f, uses, seq + i, &x)) {
                l->nz[seq[i + 1]] = x;
                absorb(l, seq[i], seq[i + 1]);
            }
        }
        /* cbr (eq or ne x, 0), the comparison right before it and used
           only there (fused into it: lower_live.c): test x */
        if (bl->ninsts >= 2 && bl->ninsts - 2 >= bl->nparams) {
            uint32_t t = bl->insts[bl->ninsts - 1],
                     c = bl->insts[bl->ninsts - 2];
            const limba_inst *ci = &f->insts[c];
            const uint32_t *oc = f->operands + ci->first;
            if (f->insts[t].op == LIMBA_OP_CBR && ci->op == LIMBA_OP_ICMP &&
                (ci->cc == LIMBA_CC_EQ || ci->cc == LIMBA_CC_NE) &&
                uses[c] == 1 && f->operands[f->insts[t].first] == c) {
                if (is_zero(f, oc[1]))
                    l->tz[c] = oc[0];
                else if (is_zero(f, oc[0]))
                    l->tz[c] = oc[1];
            }
        }
        /* an addr used once, by a load or store later in the block */
        for (i = 0; i < ns; i++) {
            uint32_t a = seq[i];
            if (f->insts[a].op != LIMBA_OP_ADDR || uses[a] != 1)
                continue;
            for (k2 = i + 1; k2 < ns; k2++) {
                uint32_t t = seq[k2];
                const limba_inst *ti = &f->insts[t];
                const uint32_t *ot = f->operands + ti->first;
                bool load =
                    is_load(ti) && ot[0] == a && ti->type != LIMBA_T_STR;
                bool store = ti->op == LIMBA_OP_STORE && ot[1] == a &&
                             ot[0] != a && f->insts[ot[0]].type != LIMBA_T_STR;
                if (load || store) {
                    l->ldx[t] = a;
                    absorb(l, a, t);
                    break;
                }
            }
        }
        /* a sub i, c used once, by the index of an addr later in the
           block */
        for (i = 0; i < ns; i++) {
            uint32_t t = seq[i];
            const limba_inst *ti = &f->insts[t];
            const uint32_t *ot = f->operands + ti->first;
            if (ti->op != LIMBA_OP_SUB || bits_of(ti->type) != 64 ||
                uses[t] != 1 || !is_const(f, ot[1]))
                continue;
            for (k2 = i + 1; k2 < ns; k2++) {
                uint32_t a = seq[k2];
                const limba_inst *ai = &f->insts[a];
                if (ai->op == LIMBA_OP_ADDR &&
                    f->operands[ai->first + 1] == t &&
                    f->operands[ai->first] != t) {
                    l->fold[a] = t;
                    /* read where the addr is read: at the load, if fused */
                    absorb(l, t, l->fu.absorbed[a] ? l->fu.anchor[a] : a);
                    break;
                }
            }
        }
    }
    /* a constant is still needed if something not absorbed uses it, or a
       range check that keeps its limits in registers */
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t ns;
        if (l->fu.absorbed[i])
            continue;
        if (l->tz[i] != LIMBA_NONE) { /* not the 0 */
            need[l->tz[i]]++;
            continue;
        }
        if (f->insts[i].op == LIMBA_OP_ADDR && addr_small(l, i)) {
            need[f->operands[f->insts[i].first]]++; /* the base alone */
            continue;
        }
        ns = meri_value_spans(f, &f->insts[i], sp);
        for (s = 0; s < ns; s++)
            for (j = 0; j < sp[s].n; j++)
                need[sp[s].o[j]]++;
        if (l->ridx[i] != LIMBA_NONE &&
            !(is_const(f, l->rlo[i]) && is_const(f, l->rhi[i]) &&
              (uint64_t)f->insts[i].imm <= 255)) {
            need[l->rlo[i]]++;
            need[l->rhi[i]]++;
        }
        if (l->ridx[i] != LIMBA_NONE)
            need[l->ridx[i]]++;
        if (l->nz[i] != LIMBA_NONE)
            need[l->nz[i]]++;
        if (l->ldx[i] != LIMBA_NONE) {
            const limba_inst *ai = &f->insts[l->ldx[i]];
            const uint32_t *oa = f->operands + ai->first;
            uint32_t ix = l->fold[l->ldx[i]] != LIMBA_NONE
                              ? f->operands[f->insts[l->fold[l->ldx[i]]].first]
                              : oa[1];
            need[oa[0]]++;
            if (!addr_small(l, l->ldx[i]))
                need[ix]++;
        }
        if (f->insts[i].op == LIMBA_OP_ADDR && l->fold[i] != LIMBA_NONE)
            need[f->operands[f->insts[l->fold[i]].first]]++;
    }
    /* hoisted: a constant used in a block on a cycle */
    for (i = 0; i < f->ninsts; i++) {
        meri_span sp[3];
        uint32_t nsp;
        uint32_t where = i; /* an absorbed one is read at its anchor */
        l->fu.alias[i] = LIMBA_NONE;
        if (l->fu.absorbed[i]) {
            if (l->fu.anchor[i] == LIMBA_NONE)
                continue;
            where = l->fu.anchor[i];
        }
        nsp = meri_value_spans(f, &f->insts[i], sp);
        for (s = 0; s < nsp; s++)
            for (j = 0; j < sp[s].n; j++)
                if (hoistable(f, sp[s].o[j]) && inloop[f->insts[where].block])
                    l->fu.hoist[sp[s].o[j]] = 2; /* wanted */
    }
    for (i = 0; i < f->ninsts; i++) {
        if ((is_const(f, i) || f->insts[i].op == LIMBA_OP_NULLV) && uses[i] &&
            !need[i])
            absorb(l, i, LIMBA_NONE);
        l->fu.hoist[i] = hoist && l->fu.hoist[i] == 2 && hoistable(f, i) &&
                         !l->fu.absorbed[i];
    }
    /* copies in the register of their source (the allocator refuses the
       parameter of a block the jumps enter while the copy is alive) */
    for (i = 0; i < f->ninsts; i++) {
        const limba_inst *in = &f->insts[i];
        uint32_t src;
        uint64_t off;
        if (in->op == LIMBA_OP_ADDR && !l->fu.absorbed[i] &&
            addr_const(l, i, &off) && off == 0) {
            l->fu.alias[i] = f->operands[in->first];
            continue;
        }
        if (in->op != LIMBA_OP_SEXT && in->op != LIMBA_OP_ZEXT &&
            in->op != LIMBA_OP_INTTOPTR && in->op != LIMBA_OP_PTRTOINT &&
            in->op != LIMBA_OP_BITCAST)
            continue;
        src = f->operands[in->first];
        if (l->fu.absorbed[i] || !pure_copy(f, in))
            continue;
        l->fu.alias[i] = src;
    }
    free(uses);
    free(need);
    free(seq);
    free(inloop);
    return true;
}

/* the address of addr a: base, index, and the scale and displacement in a
   pair of constants, with a folded sub i, c taken into them */
static void addr_parts(L *l, uint32_t a, uint32_t *base, uint32_t *idx,
                       uint32_t *kx)
{
    const limba_func *f = l->f;
    const limba_inst *ai = &f->insts[a];
    const uint32_t *oa = f->operands + ai->first;
    uint64_t scale = (uint64_t)ai->imm, disp = (uint64_t)ai->imm2;

    *base = oa[0];
    *idx = oa[1];
    if (l->fold[a] != LIMBA_NONE) {
        const uint32_t *ot = f->operands + f->insts[l->fold[a]].first;
        *idx = ot[0];
        disp -= (uint64_t)const_of(f, ot[1]) * scale; /* modulo 2^64 */
    }
    *kx = konst2(l, scale, disp);
}

static unsigned ldx_op(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
        return MERI_OP_LDX1;
    case LIMBA_T_I8:
        return MERI_OP_LDX8;
    case LIMBA_T_I16:
        return MERI_OP_LDX16;
    case LIMBA_T_I32:
        return MERI_OP_LDX32;
    case LIMBA_T_F32:
        return MERI_OP_LDXF32;
    default:
        return MERI_OP_LDX64;
    }
}

static unsigned stx_op(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
    case LIMBA_T_I8:
        return MERI_OP_STX8;
    case LIMBA_T_I16:
        return MERI_OP_STX16;
    case LIMBA_T_I32:
    case LIMBA_T_F32:
        return MERI_OP_STX32;
    default:
        return MERI_OP_STX64;
    }
}

/* a load or store with the addr before it fused in; false if none */
static bool indexed(L *l, uint32_t id, const limba_inst *in, unsigned a)
{
    const uint32_t *o = l->f->operands + in->first;
    uint32_t base, idx, kx;
    unsigned rb, ri, rv;

    uint64_t off;

    if (!l->ldx || l->ldx[id] == LIMBA_NONE)
        return false;
    if (addr_const(l, l->ldx[id], &off) && off <= 255) {
        /* base + a constant: the displacement of ld, st */
        base = l->f->operands[l->f->insts[l->ldx[id]].first];
        rb = use(l, base, 1);
        if (is_load(in))
            emit(l, meri_abc(load_op(in->type), a, rb, (unsigned)off));
        else
            emit(l, meri_abc(store_op(l->f->insts[o[0]].type), use(l, o[0], 0),
                             rb, (unsigned)off));
        return true;
    }
    addr_parts(l, l->ldx[id], &base, &idx, &kx);
    rb = use(l, base, 1);
    ri = use(l, idx, 2);
    if (is_load(in)) {
        emit(l, meri_abc(ldx_op(in->type), a, rb, ri));
    } else {
        rv = use(l, o[0], 0);
        emit(l, meri_abc(stx_op(l->f->insts[o[0]].type), rv, rb, ri));
    }
    emit(l, kx);
    return true;
}

/* a range check fused into the check; false if none */
static bool range(L *l, uint32_t id, const limba_inst *in)
{
    const limba_func *f = l->f;
    uint32_t lo, hi, x;
    unsigned ri;

    if (!l->ridx || l->ridx[id] == LIMBA_NONE)
        return false;
    lo = l->rlo[id], hi = l->rhi[id], x = l->ridx[id];
    ri = use(l, x, 0);
    if (is_const(f, lo) && is_const(f, hi) && (uint64_t)in->imm <= 255) {
        emit(l, meri_abc(MERI_OP_CHKRK, ri, (unsigned)in->imm, 0));
        emit(l,
             konst2(l, (uint64_t)const_of(f, lo), (uint64_t)const_of(f, hi)));
    } else {
        unsigned rl = use(l, lo, 1), rh = use(l, hi, 2);
        emit(l, meri_abc(MERI_OP_CHKR, ri, rl, rh));
        emit(l, konst(l, (uint64_t)in->imm));
    }
    return true;
}

/* str_ptr, str_len and print_byte as instructions of their own */
static bool rt_inline(L *l, uint32_t id, const limba_inst *in)
{
    const uint32_t *o = l->f->operands + in->first;
    unsigned x, d;

    if (in->nops != 1)
        return false;
    switch (in->imm) {
    case LIMBA_RT_STR_PTR:
    case LIMBA_RT_STR_LEN:
        x = use(l, o[0], 0);
        d = def(l, id);
        emit(l, meri_abc(in->imm == LIMBA_RT_STR_PTR ? MERI_OP_STRPTR
                                                     : MERI_OP_STRLEN,
                         d, x, 0));
        return true;
    case LIMBA_RT_PRINT_BYTE:
        emit(l, meri_abc(MERI_OP_PUTB, use(l, o[0], 0), 0, 0));
        return true;
    case LIMBA_RT_PRINT_CHAR:
        emit(l, meri_abc(MERI_OP_PUTC, use(l, o[0], 0), 0, 0));
        return true;
    }
    return false;
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
    if (l->fu.absorbed && l->fu.absorbed[id])
        return; /* emitted with a later instruction */
    if (l->fu.hoist && l->fu.hoist[id])
        return; /* loaded before block 0 */
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
        if (indexed(l, id, in, a))
            return;
        emit(l,
             meri_abc(in->type == LIMBA_T_STR ? MERI_OP_LDS : load_op(in->type),
                      a, use(l, o[0], 0), 0));
        return;
    case LIMBA_F_STORE: {
        limba_id vt = f->insts[o[0]].type;
        if (indexed(l, id, in, 0))
            return;
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
    case LIMBA_F_ADDR: {
        uint32_t base, idx, kx;
        unsigned rb, ri;
        uint64_t off;
        if (addr_const(l, id, &off) && off <= 255) {
            /* in the register of the base (fu.alias) if off is 0 */
            rb = use(l, o[0], 0);
            if (off)
                emit(l, meri_abc(MERI_OP_ADDI, a, rb, (unsigned)off));
            else
                move(l, a, rb);
            return;
        }
        addr_parts(l, id, &base, &idx, &kx);
        rb = use(l, base, 0);
        ri = use(l, idx, 1);
        emit(l, meri_abc(MERI_OP_ADDR, a, rb, ri));
        emit(l, kx);
        return;
    }
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
        if (math1(l, id, in) || rt_inline(l, id, in))
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
        if (range(l, id, in))
            return;
        emit(l,
             meri_abx(
                 MERI_OP_CHECK,
                 use(l, l->nz && l->nz[id] != LIMBA_NONE ? l->nz[id] : o[0], 0),
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

/* the order of the blocks in the code: the reverse postorder of a depth
   first walk from block 0, so that a block where paths join comes after
   the blocks that lead to it (the ends of a loop, where it jumps back,
   last in it). The walk takes the successors that leave the loop of the
   block first, so that they come after it, the loop whole before them;
   the others from the last, so that the first falls through. Blocks no
   path reaches follow, in the order of the IR. false when memory is
   exhausted */
static bool layout(const limba_func *f, uint32_t *order)
{
    uint32_t nb = f->nblocks, k, b, top, n = 0;
    uint8_t *seen = calloc(nb + 1, 1), *inloop = calloc(nb + 1, 1);
    uint32_t *comp = malloc((nb + 1) * sizeof(uint32_t));
    uint32_t *stack = malloc((nb + 1) * sizeof(uint32_t));
    uint32_t *next = malloc((nb + 1) * sizeof(uint32_t)); /* 2 passes */
    uint32_t *post = malloc((nb + 1) * sizeof(uint32_t));
    bool ok = false;

    if (!seen || !inloop || !comp || !stack || !next || !post ||
        !loop_blocks(f, inloop, comp))
        goto done;
    stack[0] = 0, next[0] = 0, top = 1, seen[0] = 1;
    while (top) {
        uint32_t v = stack[top - 1], ns = meri_nsuccs(f, v), w = LIMBA_NONE;
        /* step i of 2 * ns: first the successors out of v's component,
           then those in it, each pass from the last successor */
        while (next[top - 1] < 2 * ns && w == LIMBA_NONE) {
            uint32_t step = next[top - 1]++, pass = step / ns;
            uint32_t s2 = meri_succ(f, v, ns - 1 - step % ns);
            if (!seen[s2] && (comp[s2] != comp[v]) == (pass == 0))
                w = s2;
        }
        if (w == LIMBA_NONE) {
            post[n++] = v;
            top--;
            continue;
        }
        seen[w] = 1;
        stack[top] = w, next[top] = 0, top++;
    }
    for (k = 0; k < n; k++)
        order[k] = post[n - 1 - k];
    for (b = 0; b < nb; b++)
        if (!seen[b])
            order[n++] = b;
    ok = n == nb && order[0] == 0;
done:
    free(seen), free(inloop), free(comp);
    free(stack), free(next), free(post);
    return ok;
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

    uint32_t pass;
    uint8_t *hoist = l->fu.hoist;

    /* narrow with the constants hoisted, else narrow without, else wide
       without: a long live constant must not push a function wide */
    for (pass = 0; pass < 2; pass++) {
        l->fu.hoist = pass ? NULL : hoist;
        if (meri_alloc_regs(f, 255, 255, 0, &l->fu, &l->al)) {
            l->scratch = l->al.nregs > nparams ? l->al.nregs : nparams;
            l->outgoing = l->scratch + 1;
            if (l->outgoing + nargs <= 256)
                break;
        }
        meri_alloc_free(&l->al);
    }
    if (pass < 2) {
        if (!l->fu.hoist)
            free(hoist);
        return true;
    }
    free(hoist);
    l->fu.hoist = NULL;
    if (nparams > 250) {
        fail(l, "more than 250 parameters and 255 registers");
        return false;
    }
    l->wide = true;
    l->temp = nparams;
    l->scratch = l->temp + 4;
    if (!meri_alloc_regs(f, MERI_NOREG, l->temp, 5, &l->fu, &l->al)) {
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
    uint32_t b, i, k, *order = NULL;

    fn->type = f->type;
    fn->nparams = p->m->types[f->type].count;
    if (body && !str_ptrs_stay(&l)) {
        p->failed = true;
        return;
    }
    if (body && !find_fusions(&l)) {
        fail(&l, "out of memory");
        p->failed = true;
        return;
    }
    if (body) {
        order = malloc(((size_t)f->nblocks + 1) * sizeof(uint32_t));
        if (!order || !layout(f, order)) {
            fail(&l, "out of memory");
            p->failed = true;
            free(order);
            return;
        }
        l.fu.order = order;
    }
    if (body && !registers(&l, fn->nparams)) {
        p->failed = true;
        meri_alloc_free(&l.al);
        free(order);
        return;
    }
    if (body && !find_chklive(&l)) {
        fail(&l, "out of memory");
        p->failed = true;
        meri_alloc_free(&l.al);
        free(order);
        return;
    }
    if (body && !meri_str_plan(f, &l.sp)) {
        fail(&l, "out of memory");
        p->failed = true;
        meri_alloc_free(&l.al);
        free(order);
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
           before block 0 */
        for (i = 0; i < f->blocks[0].nparams; i++)
            if (is_str(&l, f->blocks[0].insts[i]))
                emit(&l, meri_abc(MERI_OP_SRETAIN,
                                  use(&l, f->blocks[0].insts[i], 0), 0, 0));
        /* the hoisted constants, once */
        for (i = 0; l.fu.hoist && i < f->ninsts; i++) {
            const limba_inst *in = &f->insts[i];
            uint64_t v;
            if (!l.fu.hoist[i])
                continue;
            l.cur = limba_inst_pos(f, i);
            v = in->op == LIMBA_OP_ICONST
                    ? meri_norm((uint64_t)in->imm, in->type)
                : in->op == LIMBA_OP_FCONST
                    ? (in->type == LIMBA_T_F32 ? (uint64_t)(uint32_t)in->imm
                                               : (uint64_t)in->imm)
                    : 0;
            load_const(&l, def(&l, i), v);
            done(&l);
        }
        l.order = order;
        l.at_of = malloc(((size_t)f->nblocks + 1) * sizeof(uint32_t));
        l.skip = calloc((size_t)f->nblocks + 1, 1);
        l.npred = calloc((size_t)f->nblocks + 1, sizeof(uint32_t));
        if (!l.at_of || !l.skip || !l.npred)
            fail(&l, "out of memory");
        for (k = 0; k < f->nblocks && !l.failed; k++) {
            uint32_t ns = meri_nsuccs(f, k), j;
            l.at_of[order[k]] = k;
            for (j = 0; j < ns; j++)
                l.npred[meri_succ(f, k, j)]++;
        }
        for (k = 0; k < f->nblocks && !l.failed; k++) {
            const limba_block *bl = &f->blocks[order[k]];
            uint32_t next;
            b = order[k];
            if (l.skip[b])
                continue; /* emitted with the block that jumps to it */
            l.at = k;
            next = next_block(&l);
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
    free(order);
    free(l.fix);
    meri_alloc_free(&l.al);
    meri_strplan_free(&l.sp);
    free(l.chk);
    free(l.gone);
    free(l.fu.absorbed);
    free(l.fu.hoist);
    free(l.fu.alias);
    free(l.fu.anchor);
    free(l.rlo);
    free(l.rhi);
    free(l.ridx);
    free(l.ldx);
    free(l.fold);
    free(l.nz);
    free(l.tz);
    free(l.at_of);
    free(l.skip);
    free(l.npred);
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
