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
#include <string.h>

const char *meri_version(void)
{
    return MERI_VERSION;
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
        *r = (meri_result){.status = err == EPIPE ? MERI_HALT : MERI_TRAP,
                           .code = err == EPIPE ? 141 : LIMBA_TRAP_IO};
    }
}

/* a trap of traps.def is worded by the module: its language as the
   prefix, its text or that of traps.def; everything else is Meri's own */
int meri_report(const limba_module *m, const meri_result *r, FILE *err)
{
    const char *what = NULL, *prefix = "meri";
    size_t wn = 0, pn = 4;

    switch (r->status) {
    case MERI_OK:
        return 0;
    case MERI_HALT:
        return (int)r->code;
    case MERI_TRAP:
        what = limba_trap_message(m, r->code, &wn);
        if (what && m->language != LIMBA_NONE)
            prefix = limba_str(m, m->language, &pn);
        break;
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
    }
    if (what && !wn)
        wn = strlen(what);
    if (what)
        fprintf(err, "%.*s: %.*s", (int)pn, prefix, (int)wn, what);
    else
        fprintf(err, "meri: run-time error %lld", (long long)r->code);
    if (r->pos && r->pos <= m->npos) {
        const limba_pos *p = &m->pos[r->pos - 1];
        size_t n;
        const char *file = limba_str(m, p->file, &n);
        fprintf(err, " at %.*s:%u:%u", (int)n, file, (unsigned)p->line,
                (unsigned)p->col);
    }
    fputc('\n', err);
    return 1;
}
