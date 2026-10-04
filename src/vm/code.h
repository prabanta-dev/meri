/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * code.h - the encoding of the bytecode: a word of 32 bits, the opcode in
 * the low 8 bits.
 *
 *   ABC   op | A << 8 | B << 16 | C << 24
 *   ABx   op | A << 8 | Bx << 16          (sBx: Bx with a sign)
 *   sJ    op | sJ << 8                    (24 bits with a sign)
 *
 * The formats with _X have a second word, X, read by the instruction; the
 * formats with J have a second word that is a JMP.
 */
#ifndef MERI_CODE_H
#define MERI_CODE_H

#include <stdint.h>

enum meri_fmt {
    MERI_FMT_NONE,
    MERI_FMT_A,
    MERI_FMT_AB,
    MERI_FMT_ABC,
    MERI_FMT_ABx,
    MERI_FMT_AsBx,
    MERI_FMT_sJ,
    MERI_FMT_ABC_X,
    MERI_FMT_AB_X,
    MERI_FMT_AkJ,
    MERI_FMT_ABkJ,
    MERI_FMT_MOVW, /* X = destination | source << 16 */
    MERI_FMT_BxX,  /* Bx, and X the base of the arguments */
    /* A Bx, then the arguments: a word with their number n in its low
       byte and up to 3 registers, then words of 4 registers */
    MERI_FMT_ABxN,
    MERI_FMT_ABCJ,   /* A B C, then a JMP */
    MERI_FMT_ABsC,   /* C a constant of 8 bits with a sign */
    MERI_FMT_AsBkJ,  /* B a constant of 8 bits with a sign, C the k; a JMP */
    MERI_FMT_ABxND,  /* MERI_FMT_ABxN, then a word: a register */
    MERI_FMT_ABC_XY, /* A B C, then two words X and Y */
    MERI_FMT_AKX,    /* A, then a word X: the index of a constant */
};

enum meri_op {
#define MERI_OP(name, text, format) MERI_OP_##name,
#include "vm/ops.def"
#undef MERI_OP
    MERI_OP_COUNT
};

typedef struct {
    const char *text;
    uint8_t format; /* enum meri_fmt */
} meri_op_info;

extern const meri_op_info meri_ops[MERI_OP_COUNT];

/* the words of a format (for MERI_FMT_ABxN the fewest: see meri_words) */
static inline unsigned meri_fmt_words(unsigned fmt)
{
    switch (fmt) {
    case MERI_FMT_ABC_X:
    case MERI_FMT_AB_X:
    case MERI_FMT_AkJ:
    case MERI_FMT_ABkJ:
    case MERI_FMT_MOVW:
    case MERI_FMT_BxX:
    case MERI_FMT_ABxN:
    case MERI_FMT_ABxND:
    case MERI_FMT_ABCJ:
    case MERI_FMT_AsBkJ:
    case MERI_FMT_AKX:
        return 2;
    case MERI_FMT_ABC_XY:
        return 3;
    default:
        return 1;
    }
}

/* the words of an instruction whose second word, if any, is x */
static inline unsigned meri_n_words(uint32_t n)
{
    return 2 + (n > 3 ? (n - 3 + 3) / 4 : 0);
}

#define MERI_W_OP(w) ((w) & 0xffu)
#define MERI_W_A(w) (((w) >> 8) & 0xffu)
#define MERI_W_B(w) (((w) >> 16) & 0xffu)
#define MERI_W_C(w) ((w) >> 24)
#define MERI_W_SB(w) ((int32_t)(int8_t)(uint8_t)((w) >> 16))
#define MERI_W_SC(w) ((int32_t)(int8_t)(uint8_t)((w) >> 24))
#define MERI_W_BX(w) ((w) >> 16)
#define MERI_W_SBX(w) ((int32_t)(int16_t)(uint16_t)((w) >> 16))
#define MERI_W_SJ(w) ((int32_t)(w) >> 8)

#define MERI_SJ_MAX ((1 << 23) - 1)
#define MERI_SJ_MIN (-(1 << 23))

static inline uint32_t meri_abc(unsigned op, unsigned a, unsigned b, unsigned c)
{
    return op | a << 8 | b << 16 | (uint32_t)c << 24;
}

static inline uint32_t meri_abx(unsigned op, unsigned a, unsigned bx)
{
    return op | a << 8 | (uint32_t)bx << 16;
}

static inline uint32_t meri_sj(unsigned op, int32_t sj)
{
    return op | (uint32_t)sj << 8;
}

#endif
