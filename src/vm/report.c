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

/* a trap, or an exception no handler caught (progetto_ir.md § 11i), is
   worded as the reference interpreter words it: the language of the
   module as the prefix, the name the module gives the identity, the
   message (raised with it, or the text of the module or of traps.def for
   a trap), the position; everything else is Meri's own */
int meri_report(const limba_module *m, const meri_result *r, FILE *err)
{
    const char *what = NULL, *prefix = "meri", *name = NULL;
    size_t wn = 0, pn = 4, nn = 0;

    switch (r->status) {
    case MERI_OK:
        return 0;
    case MERI_HALT:
        return (int)r->code;
    case MERI_TRAP:
        name = limba_exception_name(m, r->code, &nn);
        what = r->msg;
        wn = r->msglen;
        if (!what || (!wn && !name)) /* a trap, or raised with no words */
            what = limba_trap_message(m, r->code, &wn);
        if (m->language != LIMBA_NONE)
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
    case MERI_BADMEM:
        what = "wrong module: memory accessed with no check of nil";
        break;
    }
    if (what && !wn && r->status != MERI_TRAP)
        wn = strlen(what);
    if (name) {
        fprintf(err, "%.*s: %.*s", (int)pn, prefix, (int)nn, name);
        if (what && wn)
            fprintf(err, ": %.*s", (int)wn, what);
    } else if (what && wn) {
        fprintf(err, "%.*s: %.*s", (int)pn, prefix, (int)wn, what);
    } else if (r->status == MERI_TRAP) {
        fprintf(err, "%.*s: exception %lld", (int)pn, prefix,
                (long long)r->code);
    } else {
        fprintf(err, "meri: run-time error %lld", (long long)r->code);
    }
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
