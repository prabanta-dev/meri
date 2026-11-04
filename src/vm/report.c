/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * report.c - the end of a run as meri gives it: the output flushed, its
 * errors found there (progetto_ir.md § 11g), and how the run ended, on
 * standard error, with the exit status of the process.
 */
#include "limba/ir.h"
#include "meri/meri.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *meri_version(void)
{
    return MERI_VERSION;
}

void meri_result_free(meri_result *r)
{
    free(r->msg);
    *r = (meri_result){0};
}

void meri_flush(FILE *out, meri_result *r)
{
    errno = 0;
    if ((fflush(out) || ferror(out)) &&
        (r->status == MERI_OK || (r->status == MERI_HALT && r->code != 141))) {
        /* an error of the output found at the flush: the trap IO without
           a position, or the closed output, 141 and no message. Not after
           a trap, nor after 141 (the run already ended on a closed
           output: halt cannot give it) */
        int err = errno;
        meri_result_free(r);
        *r = (meri_result){.status = err == EPIPE ? MERI_HALT : MERI_TRAP,
                           .code = err == EPIPE ? 141 : LIMBA_TRAP_IO};
    }
}

/* a trap, or an exception no handler caught (doc/ir.md § 13.7), is
   worded by Limba, as in every engine (limba_report_uncaught): the
   language of the module as the prefix, the name the module gives the
   identity, the message (raised with it, or the text of the module or of
   traps.def for a trap), the position; everything else is the engine's
   own, prefixed with the name of the program that runs it (limba_program:
   "meri" for meri, "luxia" in luxia) */
int meri_report(const limba_module *m, const meri_result *r, FILE *err)
{
    const char *what = NULL, *prefix = limba_program();
    char reserve[96];

    switch (r->status) {
    case MERI_OK:
        return 0;
    case MERI_HALT:
        return (int)r->code;
    case MERI_TRAP:
        limba_report_uncaught(m, r->code, r->msg, r->msglen, r->pos, err);
        return 1;
    case MERI_UNREACHABLE:
        what = "unreachable executed";
        break;
    case MERI_BADCALL:
        what = "indirect call of a value that is no function of its type";
        break;
    case MERI_UNSUPPORTED:
        what = "call.ext is not supported";
        break;
    case MERI_BADENTRY:
        what = "no function main without parameters";
        break;
    case MERI_BADMEM:
        what = "wrong module: memory accessed with no check of nil";
        break;
    case MERI_NORESERVE: {
        double gib = (double)(uint64_t)r->code / (1u << 30);
        if (gib >= 1)
            snprintf(reserve, sizeof(reserve), "%.1f GiB", gib);
        else
            snprintf(reserve, sizeof(reserve), "%.0f MiB", gib * 1024);
        fprintf(err,
                "%s: cannot reserve %s for the run: the virtual memory is "
                "limited (vm.overcommit_memory, ulimit -v)\n",
                prefix, reserve);
        return 1;
    }
    default:
        fprintf(err, "%s: run-time error %lld\n", prefix, (long long)r->code);
        return 1;
    }
    fprintf(err, "%s: %s", prefix, what);
    if (r->pos && r->pos <= m->npos) {
        char at[512];
        size_t n = limba_pos_print(m, r->pos, at, sizeof(at));
        fprintf(err, " at %.*s", (int)(n < sizeof(at) ? n : sizeof(at) - 1),
                at);
    }
    fputc('\n', err);
    return 1;
}
