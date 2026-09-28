/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower.h - private to the translation of a function of the IR into
 * bytecode: the value operands of an instruction, the analysis that gives
 * every value a register (lower_live.c), the emission (lower.c).
 */
#ifndef MERI_LOWER_H
#define MERI_LOWER_H

#include "limba/ir.h"

#include <stdbool.h>
#include <stdint.h>

/* the value operands of an instruction, as up to three runs of
   f->operands (a cbr has its condition and the arguments of its two
   targets); returns how many runs */
typedef struct {
    const uint32_t *o;
    uint32_t n;
} meri_span;

uint32_t meri_value_spans(const limba_func *f, const limba_inst *in,
                          meri_span out[3]);

/* true if the instruction gives a value */
bool meri_has_value(const limba_inst *in);

/* the successors of block b, read from its terminator: how many (a
   switch counts its default and each case, repeated targets too), and the
   i-th of them */
uint32_t meri_nsuccs(const limba_func *f, limba_id b);
limba_id meri_succ(const limba_func *f, limba_id b, uint32_t i);

#define MERI_NOREG 0xffffu

/* the registers of a function */
typedef struct {
    uint16_t *reg;  /* of each value, MERI_NOREG if none */
    uint8_t *fused; /* a comparison folded into the cbr after it */
    uint32_t nregs; /* registers given: 0 .. nregs - 1 */
} meri_alloc;

/* instructions fused into a later one (lower.c): an absorbed instruction
   is not emitted and has no register; its operands are read where its
   anchor is, so they stay alive until there */
typedef struct {
    uint8_t *absorbed;
    uint32_t *anchor; /* LIMBA_NONE: itself */
    /* constants loaded once, before block 0: defined there for liveness,
       at the position 0; NULL, or no one, for none */
    uint8_t *hoist;
} meri_fusion;

/* give registers to the values of f, the parameters of block 0 first in
   0 .. n - 1 (n must not pass skip), never those in skip .. skip + nskip
   - 1; fu may be NULL; false if more than max are needed */
bool meri_alloc_regs(const limba_func *f, uint32_t max, uint32_t skip,
                     uint32_t nskip, const meri_fusion *fu, meri_alloc *a);
void meri_alloc_free(meri_alloc *a);

/* where the values of type str stop living, for their releases: a list
   of value ids for each place */
typedef struct {
    uint32_t first, n; /* in pool */
} meri_list;

typedef struct {
    uint32_t *pool;
    uint32_t npool, cappool;
    meri_list *after;  /* of each instruction: dead right after it */
    meri_list *start;  /* of each block: parameters never used */
    uint32_t *edge_at; /* of each block: its first edge in edge */
    meri_list *edge;   /* of each edge (block, successor i): dead on it */
    meri_list *at_ret; /* of each block ending in ret: alive at the ret,
                          but the value returned */
} meri_strplan;

/* the plan of f; false when memory is exhausted */
bool meri_str_plan(const limba_func *f, meri_strplan *p);
void meri_strplan_free(meri_strplan *p);

/* the canonical form of v as an integer of type t: sign-extended from its
   width, 0 or 1 for i1 */
uint64_t meri_norm(uint64_t v, limba_id t);

#endif
