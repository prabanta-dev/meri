/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * summary.c - a short account of a module of the IR: the counts of its
 * tables, then one line for each function, then how often each operation
 * occurs in the whole module.
 */
#include "summary.h"

#include <inttypes.h>

/* a string id as text, "-" for none */
static void put_name(const limba_module *m, limba_id id, FILE *out)
{
    size_t len;
    const char *s;

    if (id == LIMBA_NONE) {
        fputs("-", out);
        return;
    }
    s = limba_str(m, id, &len);
    fwrite(s, 1, len, out);
}

void meri_summary(const limba_module *m, FILE *out)
{
    uint64_t count[LIMBA_OP_COUNT] = {0};
    uint64_t ninsts = 0, nblocks = 0;
    uint32_t i, k;

    fputs("module ", out);
    put_name(m, m->name, out);
    fprintf(out, "\nmemory %s\n", m->memory == LIMBA_MEM_FB ? "fb" : "strict");
    fprintf(out,
            "strings %" PRIu32 ", types %" PRIu32 ", globals %" PRIu32
            ", externs %" PRIu32 ", functions %" PRIu32 ", positions %" PRIu32
            "\n",
            limba_str_count(m), m->ntypes, m->nglobals, m->nexterns, m->nfuncs,
            m->npos);

    for (i = 0; i < m->nfuncs; i++) {
        const limba_func *f = &m->funcs[i];

        fprintf(out, "func %" PRIu32 " ", i);
        put_name(m, f->name, out);
        fprintf(out,
                "%s: blocks %" PRIu32 ", insts %" PRIu32 ", slots %" PRIu32
                "\n",
                f->flags & LIMBA_SYM_EXPORT ? " (export)" : "", f->nblocks,
                f->ninsts, f->nslots);
        nblocks += f->nblocks;
        ninsts += f->ninsts;
        for (k = 0; k < f->ninsts; k++)
            if (f->insts[k].op < LIMBA_OP_COUNT)
                count[f->insts[k].op]++;
    }

    fprintf(out, "total: blocks %" PRIu64 ", insts %" PRIu64 "\n", nblocks,
            ninsts);
    for (k = 0; k < LIMBA_OP_COUNT; k++)
        if (count[k])
            fprintf(out, "  %-16s %" PRIu64 "\n", limba_ops[k].text, count[k]);
}
