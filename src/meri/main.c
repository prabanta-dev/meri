/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * main.c - the meri program. For now it reads a module of the IR in its
 * binary form (.lir), verifies it with the verifier of Limba and prints a
 * summary of it.
 */
#include "limba/ir.h"
#include "summary.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *out)
{
    fputs("Usage: meri [options] <input.lir>\n"
          "\n"
          "Reads a module of the IR written by limba, verifies it and\n"
          "prints a summary of it.\n"
          "\n"
          "Options\n"
          "  --summary  print the summary (the default, and for now the\n"
          "             only action)\n"
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

int main(int argc, char **argv)
{
    const char *input = NULL;
    limba_module *m;
    limba_diag d;
    uint8_t *buf;
    size_t len;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("meri %s\n", MERI_VERSION);
            return 0;
        } else if (!strcmp(a, "--summary")) {
            /* the only action for now */
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "meri: unknown option: %s\n", a);
            usage(stderr);
            return 2;
        } else if (input) {
            fprintf(stderr, "meri: one input only\n");
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

    meri_summary(m, stdout);
    limba_module_free(m);
    return 0;
}
