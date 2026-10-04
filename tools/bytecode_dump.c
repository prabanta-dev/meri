/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * bytecode_dump - the bytecode of Meri as bytes, for the proof that a
 * change leaves it as it was (tools/identity.sh).
 *
 *     bytecode_dump FILE.lir
 *
 * For each function, as meri_compile makes it: code, positions,
 * constants, window, slots and the slots to release; then the constant
 * strings. A module refused, or not compiled, writes its message instead:
 * the same for the same IR, so it compares as well.
 */
#include "limba/ir.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>

static void w(const void *p, size_t n)
{
    if (n)
        fwrite(p, 1, n, stdout);
}

int main(int argc, char **argv)
{
    limba_diag d;
    meri_diag md;
    limba_module *m;
    meri_program *p;
    uint8_t *buf;
    long len;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: bytecode_dump FILE.lir\n");
        return 2;
    }
    if (!(f = fopen(argv[1], "rb"))) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    buf = malloc(len > 0 ? len : 1);
    if (len < 0 || !buf || fread(buf, 1, len, f) != (size_t)len) {
        fprintf(stderr, "bytecode_dump: %s: not read\n", argv[1]);
        return 1;
    }
    fclose(f);
    m = limba_read(buf, len, &d);
    free(buf);
    if (!m || limba_verify(m, &d)) {
        printf("refused: %s\n", d.msg);
        limba_module_free(m);
        return 0;
    }
    if (!(p = meri_compile(m, &md))) {
        printf("not compiled: %s\n", md.msg);
        limba_module_free(m);
        return 0;
    }
    for (uint32_t i = 0; i < p->nfns; i++) {
        const meri_fn *fn = &p->fns[i];
        w(&fn->ncode, 4);
        w(fn->code, fn->ncode * 4ul);
        w(fn->pos, fn->ncode * 4ul);
        w(&fn->nk, 4);
        w(fn->k, fn->nk * 8ul);
        w(&fn->nregs, 4);
        w(&fn->nparams, 4);
        w(&fn->nslots, 4);
        w(fn->slot_off, fn->nslots * 8ul);
        w(&fn->slot_size, 8);
        w(&fn->slot_align, 8);
        w(&fn->nrel_slots, 4);
        w(fn->rel_slots, fn->nrel_slots * 4ul);
        w(fn->rel_types, fn->nrel_slots * 4ul);
    }
    w(&p->nstrs, 4);
    w(p->str_ids, p->nstrs * 4ul);
    meri_program_free(p);
    limba_module_free(m);
    return 0;
}
