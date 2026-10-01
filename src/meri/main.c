/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * main.c - the meri program: it reads a module of the IR in its binary
 * form (.lir), verifies it with the verifier of Limba, compiles it to
 * bytecode and runs it; or prints a summary of it, or its bytecode.
 */
#include "limba/ir.h"
#include "meri/meri.h"
#include "summary.h"

#include <signal.h>
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
          "  --lib-path=DIR\n"
          "             a directory where the C libraries of the program are\n"
          "             looked for before the system's (the option may be\n"
          "             given more than once)\n"
          "  --max-memory=N[K|M|G]\n"
          "             the memory the program may use (blocks, strings,\n"
          "             globals, slots); past it, \"out of memory\". By\n"
          "             default half of the physical memory\n"
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

/* a size with an optional K, M or G (powers of 1024); false if it is not
   one, 0, or past 64 bits */
static bool parse_size(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;
    unsigned shift = 0;

    if (*s < '0' || *s > '9')
        return false;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || end == s)
        return false;
    if (*end == 'K' || *end == 'M' || *end == 'G')
        shift = *end == 'K' ? 10 : *end == 'M' ? 20 : 30, end++;
    if (*end || !v || v > (UINT64_MAX >> shift))
        return false;
    *out = (uint64_t)v << shift;
    return true;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    bool summary = false, disasm = false;
    uint64_t max_memory = 0;
    limba_module *m;
    limba_diag d;
    meri_diag md;
    meri_program *p;
    meri_result r;
    meri_env env;
    uint8_t *buf;
    size_t len;
    int i, status;
    const char *libdirs[64];
    size_t nlibdirs = 0;

    for (i = 1; i < argc && !input; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("meri %s\n", meri_version());
            return 0;
        } else if (!strcmp(a, "--summary")) {
            summary = true;
        } else if (!strcmp(a, "--disasm")) {
            disasm = true;
        } else if (!strncmp(a, "--lib-path=", 11)) {
            if (nlibdirs < sizeof(libdirs) / sizeof(libdirs[0]))
                libdirs[nlibdirs++] = a + 11;
        } else if (!strncmp(a, "--max-memory=", 13)) {
            if (!parse_size(a + 13, &max_memory)) {
                fprintf(stderr, "meri: --max-memory: not a size: %s\n", a + 13);
                return 2;
            }
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
    /* every library and symbol before the first instruction */
    if (!disasm && !meri_link(p, libdirs, nlibdirs, &md)) {
        fprintf(stderr, "meri: %s: %s\n", input, md.msg);
        meri_program_free(p);
        limba_module_free(m);
        return 1;
    }
    if (disasm) {
        meri_disasm(p, stdout);
        meri_program_free(p);
        limba_module_free(m);
        return 0;
    }

    /* a closed output is EPIPE at the write, not a signal (§ 11g) */
    signal(SIGPIPE, SIG_IGN);
    env = (meri_env){argc - i, argv + i, stdin, stdout, max_memory};
    meri_run(p, "main", &env, &r);
    meri_flush(stdout, &r);
    status = meri_report(m, &r, stderr);
    meri_program_free(p);
    limba_module_free(m);
    return status;
}
