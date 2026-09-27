/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * main.c - the meri program: it reads a module of the IR in its binary
 * form (.lir), verifies it with the verifier of Limba, compiles it to
 * bytecode and runs it; or prints a summary of it, or its bytecode.
 */
#include "limba/ir.h"
#include "summary.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *out)
{
    fputs("Usage: meri [options] <input.lir> [arguments of the program]\n"
          "\n"
          "Reads a module of the IR written by limba, verifies it, compiles\n"
          "it to bytecode and runs its function main. A run-time error\n"
          "stops the program with a message and exit status 1; halt(code)\n"
          "exits with code.\n"
          "\n"
          "Options (before the input)\n"
          "  --summary  print a summary of the module instead of running it\n"
          "  --disasm   print the bytecode instead of running it\n"
          "  --version  print the version and exit\n"
          "  --help     print this text and exit\n",
          out);
}

/* the whole file into a malloc'd buffer; NULL and a message on error */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    struct stat st;
    uint8_t *buf;

    if (!fp) {
        fprintf(stderr, "meri: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX - 1) {
        fprintf(stderr, "meri: %s: not a regular file\n", path);
        fclose(fp);
        return NULL;
    }
    *len = (size_t)st.st_size;
    buf = malloc(*len + 1);
    if (!buf) {
        fprintf(stderr, "meri: %s: out of memory\n", path);
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, *len, fp) != *len || ferror(fp)) {
        fprintf(stderr, "meri: %s: read error\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

/* how the run ended, on standard error; the exit status */
static int report(const limba_module *m, const meri_result *r)
{
    const char *what = NULL;

    switch (r->status) {
    case MERI_OK:
        return 0;
    case MERI_HALT:
        return (int)r->code;
    case MERI_TRAP:
        what = limba_trap_text(r->code);
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
    if (what)
        fprintf(stderr, "meri: %s", what);
    else
        fprintf(stderr, "meri: run-time error %lld", (long long)r->code);
    if (r->pos && r->pos <= m->npos) {
        const limba_pos *p = &m->pos[r->pos - 1];
        size_t n;
        const char *file = limba_str(m, p->file, &n);
        fprintf(stderr, " at %.*s:%u:%u", (int)n, file, (unsigned)p->line,
                (unsigned)p->col);
    }
    fputc('\n', stderr);
    return 1;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    bool summary = false, disasm = false;
    limba_module *m;
    limba_diag d;
    meri_diag md;
    meri_program *p;
    meri_result r;
    meri_env env;
    uint8_t *buf;
    size_t len;
    int i, status;

    for (i = 1; i < argc && !input; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("meri %s\n", MERI_VERSION);
            return 0;
        } else if (!strcmp(a, "--summary")) {
            summary = true;
        } else if (!strcmp(a, "--disasm")) {
            disasm = true;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "meri: unknown option: %s\n", a);
            usage(stderr);
            return 2;
        } else {
            input = a;
        }
    }
    if (!input) {
        usage(stderr);
        return 2;
    }

    buf = read_file(input, &len);
    if (!buf)
        return 1;
    m = limba_read(buf, len, &d);
    free(buf);
    if (!m) {
        fprintf(stderr, "meri: %s: %s\n", input, d.msg);
        return 1;
    }
    /* a module from a file is always verified before anything else */
    if (limba_verify(m, &d) != 0) {
        fprintf(stderr, "meri: %s: invalid IR: %s\n", input, d.msg);
        limba_module_free(m);
        return 1;
    }
    if (summary) {
        meri_summary(m, stdout);
        limba_module_free(m);
        return 0;
    }

    p = meri_compile(m, &md);
    if (!p) {
        fprintf(stderr, "meri: %s: %s\n", input, md.msg);
        limba_module_free(m);
        return 1;
    }
    if (disasm) {
        meri_disasm(p, stdout);
        meri_program_free(p);
        limba_module_free(m);
        return 0;
    }

    env = (meri_env){argc - i, argv + i, stdin, stdout};
    meri_run(p, "main", &env, &r);
    status = fflush(stdout) || ferror(stdout) ? -1 : 0;
    if (status) {
        fprintf(stderr, "meri: error writing the standard output\n");
        status = 1;
    } else {
        status = report(m, &r);
    }
    meri_program_free(p);
    limba_module_free(m);
    return status;
}
