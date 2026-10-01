/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * meri.h - Meri as a library: a module of the IR of Limba compiled to
 * bytecode and run, as the program meri does with a .lir and as a program
 * that has the IR in memory (prabanta) does with the front end of Limba
 * (limba/limba_luxia.h), a function at a time.
 *
 * What a user may rely on:
 *
 * - the module is Limba's, kept by the user: it must outlive the program
 *   compiled from it, and Meri never changes it;
 * - the IR must be valid: a module from outside (a .lir) verified with
 *   limba_verify first, as meri does; one made by the front end of Limba
 *   is valid as it is made, and verified too when asked (verify, always
 *   in the builds that are not release);
 * - a function is compiled when it is given, and its body is not read
 *   again: the front end may free it right after (limba_func_clear).
 *   Every function, global and extern must be declared when the
 *   compilation begins; types, strings and positions may be added later,
 *   at the end, their ids never changing;
 * - nothing runs before meri_compile_end, and with externs nothing runs
 *   before meri_link;
 * - the output of a run is written through the FILE of meri_env: after
 *   the run, meri_flush finds the errors of the output still in its
 *   buffer, and meri_report says how the run ended as meri says it;
 * - a write to a closed output is the status 141 only if SIGPIPE is
 *   ignored (progetto_ir.md § 11g): the user ignores it before the run;
 * - out of memory while compiling is an error (meri_diag); while running,
 *   the trap NOMEM of the program.
 */
#ifndef MERI_MERI_H
#define MERI_MERI_H

#include "limba/ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* a module compiled to bytecode */
typedef struct meri_program meri_program;

/* why a compilation or a link failed */
typedef struct {
    char msg[256];
} meri_diag;

/* the version of this Meri, as meri --version prints it */
const char *meri_version(void);

/* compile a verified module whole; NULL and d filled if a function
   exceeds a limit of the engine or memory is exhausted */
meri_program *meri_compile(const limba_module *m, meri_diag *d);

/* the same a function at a time. begin when the module is declared;
   NULL and d filled on failure */
meri_program *meri_compile_begin(const limba_module *m, meri_diag *d);
/* function fid of the module, complete and verified: any order, each
   once. false and d filled on failure: then only meri_program_free */
bool meri_compile_func(meri_program *p, limba_id fid, meri_diag *d);
/* every function given: those never given are declared only, and
   calling one is unreachable. false and d filled on failure */
bool meri_compile_end(meri_program *p, meri_diag *d);

void meri_program_free(meri_program *p);

/* the platform whose C this engine calls, as module.target names it;
   NULL where it has no calling convention */
const char *meri_ffi_target(void);
/* open the libraries of p's module and resolve its externs: first in the
   directories dirs[0..ndirs), then where the system loader looks (a
   logical name "gmp" is libgmp.so; a name with a '/' or ".so" a file;
   "c", or no library, the process). false and d filled when the module is
   of another platform or anything is missing: then nothing may run */
bool meri_link(meri_program *p, const char *const *dirs, size_t ndirs,
               meri_diag *d);

/* print the bytecode of every function */
void meri_disasm(const meri_program *p, FILE *out);

enum meri_status {
    MERI_OK,          /* returned */
    MERI_TRAP,        /* a run-time error: code says which (traps.def) */
    MERI_HALT,        /* halt(code): code is the exit status */
    MERI_UNREACHABLE, /* executed unreachable */
    MERI_BADCALL,     /* call.ind of a value that is no such function */
    MERI_UNSUPPORTED, /* call.ext */
    MERI_BADENTRY,    /* no entry function, it takes parameters, or the
                         compilation has not ended */
};

typedef struct {
    int status;   /* enum meri_status */
    int64_t code; /* the trap or the halt code */
    uint32_t pos; /* where it stopped: a limba_pos index, 0 if unknown */
    uint64_t ret; /* what the entry returned, if it returned */
    /* the strings of the run still alive when it ended (those in globals
       and in blocks never freed, or a missed release) */
    uint64_t live_strings;
    uint64_t live_refs; /* the same for the numbers of BigInt */
} meri_result;

typedef struct {
    int argc; /* the arguments of the program, without its name */
    char **argv;
    FILE *in;  /* read_line, io_read */
    FILE *out; /* everything the program writes */
    /* the memory the program may use: blocks of mem_alloc, strings,
       globals and the slots of the calls alive; past it, the trap NOMEM,
       never a crash. 0 for meri_default_memory() */
    uint64_t max_memory;
} meri_env;

/* the budget of a run by default: half of the physical memory */
uint64_t meri_default_memory(void);

/* run the function called entry (no parameters, no result used) */
void meri_run(const meri_program *p, const char *entry, const meri_env *env,
              meri_result *r);

/* after the run: flush out; an error found there, on a run that had not
   already stopped, becomes the result (the trap IO without a position,
   or a closed output, 141). r as meri_run filled it */
void meri_flush(FILE *out, meri_result *r);

/* how the run ended, on err, as meri says it: nothing when it returned
   or halted; a trap in the words and the language of the module, with
   its position. The exit status of the process: 0, the code of halt, or
   1 */
int meri_report(const limba_module *m, const meri_result *r, FILE *err);

#endif
