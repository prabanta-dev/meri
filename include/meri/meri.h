/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * meri.h - Meri as a library: a module of the IR of Limba compiled to
 * bytecode and run, as the program meri does with a .lir and as a program
 * that has the IR in memory (luxia) does with a front end (Luxia's
 * limba/limba_luxia.h), a function at a time.
 *
 * What a user may rely on:
 *
 * - the module is Limba's, kept by the user: it must outlive the program
 *   compiled from it, and Meri never changes it;
 * - the IR must be valid: a module from outside (a .lir) verified with
 *   limba_verify first, as meri does; one made by the front end of Limba
 *   is valid as it is made, and verified too when asked (verify, always
 *   in the builds that are not release);
 * - the module is trusted, as a precompiled chunk is in Lua: the verifier
 *   checks its form, not that its memory accesses are safe. The checks of
 *   nil, of a block alive and of the bounds are the IR's; without them an
 *   access of nil in the first 64 KiB is MERI_BADMEM, and any other
 *   unchecked access may crash the process or change memory of the
 *   program or of Meri. A pointer of the program is checked before the C
 *   library is given it;
 * - a function is compiled when it is given, and its body is not read
 *   again: the front end may free it right after (limba_func_clear).
 *   Every function, global and extern must be declared when the
 *   compilation begins; types, strings and positions may be added later,
 *   at the end, their ids never changing;
 * - nothing runs before meri_compile_end, and with externs nothing runs
 *   before meri_link;
 * - the output of a run is written through the FILE of meri_env: after
 *   the run, meri_flush finds the errors of the output still in its
 *   buffer, and meri_report says how the run ended, prefixed with the
 *   name of the program (limba_set_program) where the module has none;
 * - a write to a closed output is the status 141 only if SIGPIPE is
 *   ignored (progetto_ir.md § 11g): the user ignores it before the run;
 * - out of memory while compiling is an error (meri_diag); while running,
 *   the trap NOMEM of the program;
 * - the engine is chosen when the compilation begins (meri_options);
 * - a program does not change once compiled and linked: meri_run only
 *   reads it, and what a run gives back (the words of an exception) is the
 *   result's, freed by meri_result_free.
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

/* the engines a program may be compiled for */
enum meri_engine {
    MERI_ENGINE_VM,  /* the interpreter of the bytecode */
    MERI_ENGINE_AOT, /* machine code made when the program is compiled:
                        not in this Meri yet, an error of the compilation */
};

/* how to compile: NULL, or every field 0, for the defaults */
typedef struct {
    int engine; /* enum meri_engine; 0: the VM */
} meri_options;

/* compile a verified module whole; NULL and d filled if the engine is not
   in this Meri, a function exceeds a limit of the engine or memory is
   exhausted */
meri_program *meri_compile(const limba_module *m, const meri_options *o,
                           meri_diag *d);

/* the same a function at a time. begin when the module is declared;
   NULL and d filled on failure */
meri_program *meri_compile_begin(const limba_module *m, const meri_options *o,
                                 meri_diag *d);
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
    MERI_TRAP,        /* a run-time error (traps.def), or an exception no
                         handler caught: code is its identity */
    MERI_HALT,        /* halt(code): code is the exit status */
    MERI_UNREACHABLE, /* executed unreachable */
    MERI_BADCALL,     /* call.ind of a value that is no such function */
    MERI_UNSUPPORTED, /* call.ext */
    MERI_BADENTRY,    /* no entry function, it takes parameters, or the
                         compilation has not ended */
    MERI_BADMEM,      /* memory touched in its first 64 KiB, or past its
                         end, by an access the IR should have checked
                         first (nil): a wrong module, never a crash */
    MERI_NORESERVE,   /* the stacks of the run could not be reserved, even
                         smaller (code: the bytes last asked): the system
                         limits the virtual memory (vm.overcommit_memory=2,
                         ulimit -v). Nothing of the program ran */
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
    /* the words of an exception no handler caught (msglen bytes, not
       terminated): raised with them, or a trap's made when a handler
       caught it and raised it again. The result's own: meri_result_free
       frees it. NULL for a trap no handler saw (and a raise of the str
       0): its words are those of the module or of traps.def */
    char *msg;
    size_t msglen;
} meri_result;

/* free what a result owns (msg) and zero it; any number of times. A
   result is freed before meri_run fills it again */
void meri_result_free(meri_result *r);

typedef struct {
    int argc; /* the arguments of the program, without its name */
    char **argv;
    FILE *in;  /* read_line, io_read */
    FILE *out; /* everything the program writes */
    /* the memory the program may use: blocks of mem_alloc, strings,
       globals and the slots of the calls alive; past it, the trap NOMEM,
       never a crash. 0 for meri_default_memory() */
    uint64_t max_memory;
    /* the workers of a par_for, at most 255 (more are 255); 0: the
       cores, at most 255. This Meri runs a par_for in sequence, whatever
       the number */
    unsigned threads;
} meri_env;

/* the budget of a run by default: half of the physical memory */
uint64_t meri_default_memory(void);

/* run the function called entry (no parameters, no result used); p is
   only read. r is filled whole: what it owned before is not freed */
void meri_run(const meri_program *p, const char *entry, const meri_env *env,
              meri_result *r);

/* after the run: flush out; an error found there, on a run that had not
   already stopped, becomes the result (the trap IO without a position,
   or a closed output, 141). r as meri_run filled it */
void meri_flush(FILE *out, meri_result *r);

/* how the run ended, on err: nothing when it returned or halted; a trap
   or an exception not caught in the words and the language of the module,
   with its position; an error of the engine (unreachable, a bad call.ind,
   MERI_BADMEM, MERI_NORESERVE...) prefixed with the name the program gave
   Limba (limba_set_program), as for a trap of a module with no language. The
   exit status of the process: 0, the code of halt, or 1 */
int meri_report(const limba_module *m, const meri_result *r, FILE *err);

#endif
