/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_kwide - the constants of a function past the first 65 536.
 *
 * Each program is written here, as text of the IR: main first sums N
 * constants all different, too large for LOADI, so that every one is in
 * the pool of main; what follows meets a pool already past 65 536, and
 * the trap codes are new there: LOADK, CHECK, CHKLIVE, CHKNL and TRAP in
 * their wide form, and the range checks that keep the trap in 16 bits
 * (CHKADDR, CHKADDRS) not fused. Each run is checked for its output and
 * its end, and the disassembly of main for the forms it must have.
 */
#define _GNU_SOURCE
#include "limba/ir.h"
#include "meri/meri.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N
#define N 70000 /* -DN=3: the same programs, fused */
#endif
#define K ((int64_t)1 << 40) /* the first constant */

static int failures, cases;

#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            failures++;                                                        \
            fprintf(stderr, "test_kwide: %s: ", name);                         \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* the end of each program, after the sum in v<2N>: its first value is
   v<2N + 1>, written $ (and $1, $2... the ones after it) */
typedef struct {
    const char *name;
    const char *tail;
    int status;      /* MERI_OK or MERI_TRAP */
    int64_t code;    /* the trap */
    const char *has; /* in the disassembly, separated by blanks */
    const char *hasnt;
} prog;

/* the block b of 32 bytes: lo 1, hi 3, the index in $9 at 16 */
#define BLOCK(i)                                                               \
    "  $0 = iconst i64 32\n"                                                   \
    "  $1 = call.rt ptr mem_alloc($0)\n"                                       \
    "  $2 = iconst i64 1\n"                                                    \
    "  store $2, $1\n"                                                         \
    "  $3 = iconst i64 8\n"                                                    \
    "  $4 = addr ptr $1, $3, 1, 0\n"                                           \
    "  $5 = iconst i64 3\n"                                                    \
    "  store $5, $4\n"                                                         \
    "  $6 = iconst i64 16\n"                                                   \
    "  $7 = addr ptr $1, $6, 1, 0\n"                                           \
    "  $8 = iconst i64 " i "\n"                                                \
    "  store $8, $7\n"                                                         \
    "  $10 = load i64 $1\n"                                                    \
    "  $11 = load i64 $4\n"                                                    \
    "  $9 = load i64 $7\n"                                                     \
    "  $12 = icmp.sge i1 $9, $10\n"                                            \
    "  $13 = icmp.sle i1 $9, $11\n"                                            \
    "  $14 = and i1 $12, $13\n"                                                \
    "  check $14, 100\n"

static const prog progs[] = {
    {"checks pass",
     "  $20 = null ptr\n"
     "  $21 = iconst i64 16\n"
     "  $22 = call.rt ptr mem_alloc($21)\n"
     "  $23 = iconst i64 7\n"
     "  store $23, $22\n"
     "  $24 = icmp.ne i1 $22, $20\n"
     "  check $24, 102\n"
     "  $25 = call.rt i1 ptr_live($22)\n"
     "  check $25, 105\n"
     "  $26 = load i64 $22\n"
     "  call.rt void print_i64($26)\n"
     "  call.rt void print_nl()\n"
     "  $27 = call.rt i1 ptr_live($22)\n"
     "  check $27, 105\n"
     "  store $26, $22\n"
     "  $28 = iconst i64 0\n"
     "  $29 = icmp.ne i1 $26, $28\n"
     "  check $29, 101\n"
     "  ret $28\n",
     MERI_OK, 0, "loadkw chknlw chklivew checkw", NULL},
    {"trap", "  trap 103\n", MERI_TRAP, 103, "trapw", NULL},
    {"check fails",
     "  $20 = iconst i64 0\n"
     "  $21 = icmp.eq i1 $20, $20\n"
     "  check $21, 101\n"
     "  $22 = icmp.ne i1 $20, $20\n"
     "  check $22, 104\n"
     "  ret $20\n",
     MERI_TRAP, 104, "checkw", NULL},
    {"dangling",
     "  $20 = iconst i64 16\n"
     "  $21 = call.rt ptr mem_alloc($20)\n"
     "  call.rt void mem_free($21)\n"
     "  $22 = call.rt i1 ptr_live($21)\n"
     "  check $22, 105\n"
     "  $23 = load i64 $21\n"
     "  ret $23\n",
     MERI_TRAP, 105, "chklivew", NULL},
    {"nil",
     "  $20 = null ptr\n"
     "  $21 = icmp.ne i1 $20, $20\n"
     "  check $21, 102\n"
     "  $22 = call.rt i1 ptr_live($20)\n"
     "  check $22, 105\n"
     "  $23 = load i64 $20\n"
     "  ret $23\n",
     MERI_TRAP, 102, "chknlw", NULL},
    {"not nil, dangling",
     "  $20 = null ptr\n"
     "  $21 = iconst i64 16\n"
     "  $22 = call.rt ptr mem_alloc($21)\n"
     "  call.rt void mem_free($22)\n"
     "  $23 = icmp.ne i1 $22, $20\n"
     "  check $23, 102\n"
     "  $24 = call.rt i1 ptr_live($22)\n"
     "  check $24, 105\n"
     "  $25 = load i64 $22\n"
     "  ret $25\n",
     MERI_TRAP, 105, "chknlw", NULL},
    {"range, sub and addr",
     BLOCK("2") "  $15 = sub i64 $9, $10\n"
                "  $16 = addr ptr $1, $15, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $16\n"
                "  call.rt void print_i64($17)\n"
                "  call.rt void print_nl()\n"
                "  $18 = iconst i64 0\n"
                "  ret $18\n",
     MERI_OK, 0, "chkr", "chkaddrs chkrs"},
    {"range, sub and addr: trap",
     BLOCK("5") "  $15 = sub i64 $9, $10\n"
                "  $16 = addr ptr $1, $15, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $16\n"
                "  ret $17\n",
     MERI_TRAP, 100, "chkr", "chkaddrs chkrs"},
    {"range, sub and two addrs",
     BLOCK("2") "  $15 = sub i64 $9, $10\n"
                "  $16 = addr ptr $1, $15, 8, 0\n"
                "  $19 = addr ptr $7, $15, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $19\n"
                "  $20 = load i64 $19\n"
                "  call.rt void print_i64($20)\n"
                "  call.rt void print_nl()\n"
                "  $18 = iconst i64 0\n"
                "  ret $18\n",
     MERI_OK, 0, "chkr", "chkaddrs chkrs"},
    {"range, sub and two addrs: trap",
     BLOCK("4") "  $15 = sub i64 $9, $10\n"
                "  $16 = addr ptr $1, $15, 8, 0\n"
                "  $19 = addr ptr $7, $15, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $19\n"
                "  ret $17\n",
     MERI_TRAP, 100, "chkr", "chkaddrs chkrs"},
    {"range and addr",
     BLOCK("2") "  $16 = addr ptr $1, $9, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $16\n"
                "  call.rt void print_i64($17)\n"
                "  call.rt void print_nl()\n"
                "  $18 = iconst i64 0\n"
                "  ret $18\n",
     MERI_OK, 0, "chkr", "chkaddr"},
    {"range and addr: trap",
     BLOCK("0") "  $16 = addr ptr $1, $9, 8, 0\n"
                "  $17 = load i64 $16\n"
                "  store $17, $16\n"
                "  ret $17\n",
     MERI_TRAP, 100, "chkr", "chkaddr"},
};

/* what each program prints after the sum */
static const char *const after[] = {"7\n", "", "",    "", "",    "",
                                    "3\n", "", "3\n", "", "2\n", ""};

/* the text of program g: $k is v<2N + 1 + k> */
static char *text(const prog *g, size_t *len)
{
    char *buf = NULL;
    FILE *f = open_memstream(&buf, len);
    const char *t;
    int i;

    fputs("func @main : fn() -> i64 {\nb0:\n  v0 = iconst i64 0\n", f);
    for (i = 0; i < N; i++)
        fprintf(f, "  v%d = iconst i64 %" PRId64 "\n  v%d = add i64 v%d, v%d\n",
                2 * i + 1, K + i, 2 * i + 2, 2 * i, 2 * i + 1);
    fprintf(f, "  call.rt void print_i64(v%d)\n  call.rt void print_nl()\n",
            2 * N);
    for (t = g->tail; *t; t++) {
        if (*t == '$') {
            int k = (int)strtol(t + 1, (char **)&t, 10);
            fprintf(f, "v%d", 2 * N + 1 + k);
            t--;
        } else {
            fputc(*t, f);
        }
    }
    fputs("}\n", f);
    fclose(f);
    return buf;
}

/* each word of list (blanks between) found in s, or none of them */
static bool all_in(const char *s, const char *list, bool want)
{
    char w[32];
    int n;

    while (list && sscanf(list, "%31s%n", w, &n) == 1) {
        char pat[40];
        snprintf(pat, sizeof(pat), "  %s ", w);
        if ((strstr(s, pat) != NULL) != want)
            return false;
        list += n;
    }
    return true;
}

static void run_one(const prog *g, const char *expect)
{
    const char *name = g->name;
    size_t len, on = 0, dn = 0;
    char *src = text(g, &len), *ob = NULL, *db = NULL;
    limba_diag d;
    meri_diag md;
    limba_module *m = limba_parse(src, len, &d);
    meri_program *p;
    meri_result r;
    FILE *out, *dis;

    cases++;
    free(src);
    if (!m || limba_verify(m, &d)) {
        CHECK(0, "IR refused: %s", d.msg);
        limba_module_free(m);
        return;
    }
    p = meri_compile(m, &md);
    CHECK(p, "not compiled: %s", md.msg);
    if (!p) {
        limba_module_free(m);
        return;
    }
    dis = open_memstream(&db, &dn);
    meri_disasm(p, dis);
    fclose(dis);
    CHECK(all_in(db, g->has, true), "the disassembly lacks one of: %s", g->has);
    CHECK(all_in(db, g->hasnt, false), "the disassembly has one of: %s",
          g->hasnt);
    out = open_memstream(&ob, &on);
    meri_run(p, "main", &(meri_env){0, NULL, stdin, out, 0}, &r);
    meri_flush(out, &r);
    fclose(out);
    CHECK(r.status == g->status, "status %d, not %d", r.status, g->status);
    CHECK(g->status != MERI_TRAP || r.code == g->code,
          "trap %" PRId64 ", not %" PRId64, r.code, g->code);
    CHECK(!strcmp(ob, expect), "printed \"%s\", not \"%s\"", ob, expect);
    free(ob);
    free(db);
    meri_program_free(p);
    limba_module_free(m);
}

int main(void)
{
    char sum[64];
    size_t i;

    /* K * N + 0 + 1 + ... + N - 1 */
    snprintf(sum, sizeof(sum), "%" PRId64 "\n",
             K * N + (int64_t)N * (N - 1) / 2);
    for (i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        char expect[128];
        snprintf(expect, sizeof(expect), "%s%s", sum, after[i]);
        run_one(&progs[i], expect);
    }
    printf("test_kwide: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
