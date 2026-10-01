/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * vm.h - the virtual machine of Meri inside: the bytecode of a module
 * (meri_compile, meri/meri.h), as the interpreter runs it (meri_run).
 *
 * The strings count their references (progetto_ir.md § 11c); a block of
 * mem_alloc goes back to malloc when freed, its pointers tagged with its
 * generation (heap.h). A function may have up to 65535 registers: past
 * 256 it is emitted wide (lower.c).
 */
#ifndef MERI_VM_H
#define MERI_VM_H

#include "limba/ir.h"
#include "meri/meri.h"
#include "vm/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* a function in bytecode */
typedef struct {
    uint32_t *code;
    uint32_t ncode;
    uint32_t *pos; /* the position (limba_pos index) of every word, or 0 */
    uint64_t *k;   /* constants */
    uint32_t nk;
    uint32_t nregs; /* the window: registers, scratch, outgoing arguments */
    uint32_t nparams;
    uint64_t *slot_off; /* offset of each slot in the frame's slot area */
    uint32_t nslots;
    uint64_t slot_size;  /* bytes of the slot area */
    uint64_t slot_align; /* its alignment */
    uint32_t *rel_slots; /* the typed slots holding a str: released at ret */
    limba_id *rel_types; /* the type of each: the body is gone by then */
    uint32_t nrel_slots;
    limba_id type; /* the function type in the module */
} meri_fn;

struct meri_program {
    const limba_module *m; /* kept for names, types and positions */
    meri_fn *fns;
    uint32_t nfns;
    meri_str **strs;   /* the string constants, immortal */
    limba_id *str_ids; /* the string of the module each one holds */
    uint32_t nstrs, capstrs;
    uint8_t *holds_str; /* of each type of the module: has a str in it */
    uint32_t ntypes;    /* the types holds_str knows */
    bool failed;        /* while compiling */
    bool ended;         /* every function compiled: it may run */
    /* the externs, resolved by meri_link; NULL before */
    struct meri_ext *exts;
    uint32_t nexts;
    void **libs; /* the libraries opened */
    limba_id *libnames;
    uint32_t nlibs;
};

#endif
