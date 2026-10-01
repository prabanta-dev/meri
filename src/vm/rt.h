/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * rt.h - the run-time library: the functions of runtime.def, and the state
 * of a run they share with the interpreter.
 */
#ifndef MERI_RT_H
#define MERI_RT_H

#include "vm/heap.h"
#include "vm/str.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

typedef struct {
    const meri_program *p;
    const meri_env *env;
    meri_heap heap;
    int status;      /* enum meri_status, when a call stops the run */
    int64_t code;    /* its trap or halt code */
    meri_str **strs; /* the strings of the run alive, freed at the end */
    size_t nstrs, capstrs;
    struct meri_big **bigs; /* the numbers of the run alive (big.c) */
    size_t nbigs, capbigs;
    /* the memory of the program (blocks of mem_alloc, strings, globals,
       the slots of the calls alive) against its budget: past it, NOMEM */
    uint64_t used, budget;
    /* the end of the input was seen: from then on read_line is false and
       io_read 0, whatever comes after (progetto_ir.md § 11g) */
    bool eof;
    /* the run stopped where no position is told: an error of the output,
       found at a later write or at the flush */
    bool nopos;
} meri_state;

/* n bytes more of the program's memory; false, and nothing counted, if
   they do not fit in its budget */
static inline bool meri_state_take(meri_state *s, uint64_t n)
{
    if (n > s->budget || s->used > s->budget - n)
        return false;
    s->used += n;
    return true;
}

static inline void meri_state_give(meri_state *s, uint64_t n)
{
    s->used -= n < s->used ? n : s->used;
}

/* a string of the run, n bytes from p (p may be NULL for 0), with one
   reference, the caller's; NULL when memory is exhausted */
meri_str *meri_state_str(meri_state *s, const char *p, size_t n);
/* a string of the run of n bytes, written by the caller */
meri_str *meri_state_alloc(meri_state *s, size_t n);
/* one reference less on the string of v; the last one frees it */
void meri_state_release(meri_state *s, uint64_t v);
/* an immortal string of the run (an initial value of a global) */
meri_str *meri_state_immortal(meri_state *s, const char *p, size_t n);
/* retain (d = 1) or release (d = -1) every str of n values of type t
   at p */
void meri_state_rc(meri_state *s, uint64_t p, limba_id t, uint64_t n, int d);
void meri_state_free_strs(meri_state *s);

/* call runtime function id with the arguments a[0..n); the result, if
   any, goes to a[0]. false when the run stops: s->status says why */
bool meri_rt_call(meri_state *s, uint32_t id, uint64_t *a);

/* stop the run with the trap code: always false */
bool meri_rt_trap(meri_state *s, int64_t code);
/* stop the run for an error of the output, errno err: a closed output
   (EPIPE) ends it with status 141 and no message, any other is the trap
   IO without a position (§ 11g): always false */
bool meri_rt_out_error(meri_state *s, int err);

#endif
