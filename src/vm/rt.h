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
#include <stdint.h>

typedef struct {
    const meri_program *p;
    const meri_env *env;
    meri_heap heap;
    int status;      /* enum meri_status, when a call stops the run */
    int64_t code;    /* its trap or halt code */
    meri_str **strs; /* every string made while running, freed at the end */
    size_t nstrs, capstrs;
    /* the memory of the program (blocks of mem_alloc, strings, globals,
       the slots of the calls alive) against its budget: past it, NOMEM */
    uint64_t used, budget;
} meri_state;

/* n bytes more of the program's memory; false, and nothing counted, if
   they do not fit in its budget */
bool meri_state_take(meri_state *s, uint64_t n);
void meri_state_give(meri_state *s, uint64_t n);

/* a string of the run, n bytes from p (p may be NULL for 0); NULL when
   memory is exhausted */
meri_str *meri_state_str(meri_state *s, const char *p, size_t n);
/* a string of the run of n bytes, written by the caller */
meri_str *meri_state_alloc(meri_state *s, size_t n);
void meri_state_free_strs(meri_state *s);

/* call runtime function id with the arguments a[0..n); the result, if
   any, goes to a[0]. false when the run stops: s->status says why */
bool meri_rt_call(meri_state *s, uint32_t id, uint64_t *a);

/* stop the run with the trap code: always false */
bool meri_rt_trap(meri_state *s, int64_t code);

#endif
