/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_vm.c - the virtual machine against what each program says it does.
 * Run from the root of the repository (build.sh test does); it reads
 * tests/vm and the cases of the reference interpreter of Limba
 * (MERI_LIMBA_DIR/tests/eval), and writes nothing.
 *
 * Every .lit there starts with its expectations, one comment a line:
 *   "; output: <line>"  a line it prints, in order
 *   "; result: <n>"     what main returns
 *   "; trap: <code>"    the run-time error it stops with
 *   "; at: <l>:<c>"     where (tests/vm only)
 *   "; memory: broken"  a rule of the strings in memory broken: the first
 *                       cut of Meri does not check them, so it is skipped
 * The program is compiled, disassembled (to nothing) and run.
 */
#define _GNU_SOURCE
#include "limba/ir.h"
#include "vm/vm.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, cases, skipped;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_vm: " __VA_ARGS__);                          \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) ||
        !(buf = malloc((size_t)n + 1))) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

typedef struct {
    char out[4096];
    size_t outlen;
    int status;
    int64_t value; /* the trap code or the result */
    bool has_result, has_at, skip;
    unsigned line, col;
} expect;

static void header(const char *text, expect *e)
{
    const char *p;

    memset(e, 0, sizeof(*e));
    e->status = MERI_OK;
    for (p = text; *p == ';';) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (!strncmp(p, "; output: ", 10) && e->outlen + n < sizeof(e->out)) {
            memcpy(e->out + e->outlen, p + 10, n - 10);
            e->outlen += n - 10;
            e->out[e->outlen++] = '\n';
        } else if (!strncmp(p, "; result: ", 10)) {
            e->value = strtoll(p + 10, NULL, 10);
            e->has_result = true;
        } else if (!strncmp(p, "; trap: ", 8)) {
            e->status = MERI_TRAP;
            e->value = strtoll(p + 8, NULL, 10);
        } else if (!strncmp(p, "; at: ", 6)) {
            e->has_at = sscanf(p + 6, "%u:%u", &e->line, &e->col) == 2;
        } else if (!strncmp(p, "; memory: broken", 16)) {
            e->skip = true;
        }
        if (!eol)
            break;
        p = eol + 1;
    }
}

static void one(const char *path)
{
    size_t len;
    char *text = slurp(path, &len), *out = NULL;
    size_t outlen = 0;
    limba_diag d = {{0}, 0};
    limba_module *m;
    meri_program *p;
    meri_diag md;
    meri_result r;
    meri_env env;
    expect e;
    FILE *null, *o, *in;

    if (!text) {
        CHECK(false, "%s: cannot read", path);
        return;
    }
    header(text, &e);
    if (e.skip) {
        skipped++;
        free(text);
        return;
    }
    cases++;
    m = limba_parse(text, len, &d);
    free(text);
    if (!m || limba_verify(m, &d) != 0) {
        CHECK(false, "%s: %s", path, d.msg);
        limba_module_free(m);
        return;
    }
    p = meri_compile(m, &md);
    if (!p) {
        CHECK(false, "%s: %s", path, md.msg);
        limba_module_free(m);
        return;
    }
    null = fopen("/dev/null", "w");
    if (null) {
        meri_disasm(p, null);
        fclose(null);
    }
    o = open_memstream(&out, &outlen);
    in = fopen("/dev/null", "r");
    if (!o || !in) {
        CHECK(false, "%s: no stream", path);
    } else {
        env = (meri_env){0, NULL, in, o};
        meri_run(p, "main", &env, &r);
        fclose(o);
        o = NULL;
        CHECK(r.status == e.status, "%s: ended with status %d, expected %d",
              path, r.status, e.status);
        CHECK(e.status != MERI_TRAP || r.code == e.value,
              "%s: trap %" PRId64 ", expected %" PRId64, path, r.code, e.value);
        CHECK(!e.has_result || (int64_t)r.ret == e.value,
              "%s: returned %" PRId64 ", expected %" PRId64, path,
              (int64_t)r.ret, e.value);
        CHECK(outlen == e.outlen && !memcmp(out, e.out, outlen),
              "%s: printed\n%.*s---\nexpected\n%.*s", path, (int)outlen, out,
              (int)e.outlen, e.out);
        if (e.has_at) {
            const limba_pos *ps =
                r.pos && r.pos <= m->npos ? &m->pos[r.pos - 1] : NULL;
            CHECK(ps && ps->line == e.line && ps->col == e.col,
                  "%s: stopped at %u:%u, expected %u:%u", path,
                  ps ? (unsigned)ps->line : 0, ps ? (unsigned)ps->col : 0,
                  e.line, e.col);
        }
    }
    if (o)
        fclose(o);
    if (in)
        fclose(in);
    free(out);
    meri_program_free(p);
    limba_module_free(m);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *ent;
    char **names = NULL;
    size_t n = 0, cap = 0, i;

    if (!d) {
        CHECK(false, "%s: cannot open (run from the root)", path);
        return;
    }
    while ((ent = readdir(d))) {
        size_t k = strlen(ent->d_name);
        char *full;
        if (k < 5 || strcmp(ent->d_name + k - 4, ".lit"))
            continue;
        if (n == cap) {
            char **q;
            cap = cap ? 2 * cap : 32;
            q = realloc(names, cap * sizeof(*names));
            if (!q)
                break;
            names = q;
        }
        full = malloc(strlen(path) + k + 2);
        if (!full)
            break;
        sprintf(full, "%s/%s", path, ent->d_name);
        names[n++] = full;
    }
    closedir(d);
    qsort(names, n, sizeof(*names), by_name);
    for (i = 0; i < n; i++) {
        one(names[i]);
        free(names[i]);
    }
    free(names);
}

int main(void)
{
    dir("tests/vm");
    dir(MERI_LIMBA_DIR "/tests/eval");
    printf("test_vm: %d cases, %d skipped, %d failures\n", cases, skipped,
           failures);
    return failures ? 1 : 0;
}
