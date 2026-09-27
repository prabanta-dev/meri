/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * disasm.c - the bytecode as text, a function at a time: the word, its
 * instruction and operands, and the source position it comes from.
 */
#include "vm/code.h"
#include "vm/vm.h"

#include <inttypes.h>

static void operands(FILE *out, const meri_fn *fn, uint32_t at)
{
    uint32_t w = fn->code[at], x = at + 1 < fn->ncode ? fn->code[at + 1] : 0;
    unsigned f = meri_ops[MERI_W_OP(w)].format;

    switch (f) {
    case MERI_FMT_NONE:
        break;
    case MERI_FMT_A:
        fprintf(out, " r%u", MERI_W_A(w));
        break;
    case MERI_FMT_AB:
        fprintf(out, " r%u, r%u", MERI_W_A(w), MERI_W_B(w));
        break;
    case MERI_FMT_ABC:
        fprintf(out, " r%u, r%u, r%u", MERI_W_A(w), MERI_W_B(w), MERI_W_C(w));
        break;
    case MERI_FMT_ABx:
        fprintf(out, " r%u, %u", MERI_W_A(w), MERI_W_BX(w));
        if (MERI_W_OP(w) == MERI_OP_LOADK && MERI_W_BX(w) < fn->nk)
            fprintf(out, " ; 0x%" PRIx64, fn->k[MERI_W_BX(w)]);
        break;
    case MERI_FMT_AsBx:
        fprintf(out, " r%u, %d", MERI_W_A(w), (int)MERI_W_SBX(w));
        break;
    case MERI_FMT_sJ:
        fprintf(out, " -> %" PRId64, (int64_t)at + 1 + MERI_W_SJ(w));
        break;
    case MERI_FMT_ABC_X:
        fprintf(out, " r%u, r%u, r%u, x=0x%" PRIx32, MERI_W_A(w), MERI_W_B(w),
                MERI_W_C(w), x);
        break;
    case MERI_FMT_AB_X:
        fprintf(out, " r%u, r%u, x=0x%" PRIx32, MERI_W_A(w), MERI_W_B(w), x);
        break;
    case MERI_FMT_AkJ:
        fprintf(out, " r%u, k=%u -> %" PRId64, MERI_W_A(w), MERI_W_B(w),
                (int64_t)at + 2 + MERI_W_SJ(x));
        break;
    case MERI_FMT_MOVW:
        fprintf(out, " r%" PRIu32 ", r%" PRIu32, x & 0xffff, x >> 16);
        break;
    case MERI_FMT_BxX:
        fprintf(out, " r%" PRIu32 ", %u", x, MERI_W_BX(w));
        break;
    case MERI_FMT_ABkJ:
        fprintf(out, " r%u, r%u, k=%u -> %" PRId64, MERI_W_A(w), MERI_W_B(w),
                MERI_W_C(w), (int64_t)at + 2 + MERI_W_SJ(x));
        break;
    }
}

void meri_disasm(const meri_program *p, FILE *out)
{
    uint32_t i, at;

    for (i = 0; i < p->nfns; i++) {
        const meri_fn *fn = &p->fns[i];
        size_t n;
        const char *name = limba_str(p->m, p->m->funcs[i].name, &n);

        fprintf(out,
                "func %" PRIu32 " %.*s: params %" PRIu32 ", registers %" PRIu32
                ", slots %" PRIu64 " bytes, words %" PRIu32 "\n",
                i, (int)n, name, fn->nparams, fn->nregs, fn->slot_size,
                fn->ncode);
        for (at = 0; at < fn->ncode;) {
            uint32_t w = fn->code[at];
            unsigned op = MERI_W_OP(w);
            if (op >= MERI_OP_COUNT) {
                fprintf(out, "%6" PRIu32 "  0x%08" PRIx32 " ?\n", at, w);
                at++;
                continue;
            }
            fprintf(out, "%6" PRIu32 "  %s", at, meri_ops[op].text);
            operands(out, fn, at);
            if (fn->pos[at] && fn->pos[at] <= p->m->npos) {
                const limba_pos *ps = &p->m->pos[fn->pos[at] - 1];
                fprintf(out, "  ; %" PRIu32 ":%" PRIu32, ps->line, ps->col);
            }
            fputc('\n', out);
            at += meri_fmt_words(meri_ops[op].format);
        }
    }
}
