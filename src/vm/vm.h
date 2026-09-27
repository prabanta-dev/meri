/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * vm.h - the virtual machine of Meri: a module of the IR compiled to
 * bytecode (meri_compile), and its execution (meri_run).
 *
 * First cut (job/plans/primo_motore.md): no reference counts, memory of
 * mem_alloc never reused, at most 256 registers a function.
 */
#ifndef MERI_VM_H
#define MERI_VM_H

#include "limba/ir.h"
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
    uint32_t *slot_off; /* offset of each slot in the frame's slot area */
    uint32_t nslots;
    uint32_t slot_size;  /* bytes of the slot area */
    uint32_t slot_align; /* its alignment */
    limba_id type;       /* the function type in the module */
} meri_fn;

typedef struct {
    const limba_module *m; /* kept for names, types and positions */
    meri_fn *fns;
    uint32_t nfns;
    meri_str **strs;   /* the string constants, immortal */
    limba_id *str_ids; /* the string of the module each one holds */
    uint32_t nstrs, capstrs;
    bool failed; /* while compiling */
} meri_program;

/* a function the compiler cannot translate: which one and why */
typedef struct {
    char msg[256];
} meri_diag;

/* compile a verified module; NULL and d filled if a function exceeds a
   limit of the first cut */
meri_program *meri_compile(const limba_module *m, meri_diag *d);
void meri_program_free(meri_program *p);

/* print the bytecode of every function */
void meri_disasm(const meri_program *p, FILE *out);

enum meri_status {
    MERI_OK,          /* returned */
    MERI_TRAP,        /* a run-time error: code says which (traps.def) */
    MERI_HALT,        /* halt(code): code is the exit status */
    MERI_UNREACHABLE, /* executed unreachable */
    MERI_BADCALL,     /* call.ind of a value that is no such function */
    MERI_UNSUPPORTED, /* call.ext */
    MERI_BADENTRY,    /* no entry function, or it takes parameters */
};

typedef struct {
    int status;   /* enum meri_status */
    int64_t code; /* the trap or the halt code */
    uint32_t pos; /* where it stopped: a limba_pos index, 0 if unknown */
    uint64_t ret; /* what the entry returned, if it returned */
} meri_result;

typedef struct {
    int argc; /* the arguments of the program, without its name */
    char **argv;
    FILE *in;  /* read_line, input_line */
    FILE *out; /* everything the program writes */
} meri_env;

/* run the function called entry (no parameters, no result used) */
void meri_run(const meri_program *p, const char *entry, const meri_env *env,
              meri_result *r);

#endif
