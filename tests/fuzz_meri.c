/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fuzz_meri.c - fuzzing target of the compiler of Meri: whatever the
 * bytes, a module that reads (.lir, or .lit) and verifies must compile, or
 * be refused with a message, and disassemble, without a crash. It is not
 * run: a module of the IR may touch any address (memory FB).
 *
 * With clang: clang -fsanitize=fuzzer,address -DMERI_FUZZER ... builds a
 * libFuzzer program. Without, it is a driver that runs the files named on
 * its command line through the same function, to replay a corpus or a
 * crash.
 */
#include "limba/ir.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static FILE *null;
    limba_module *m;
    meri_program *p;
    meri_diag d;

    if (!null)
        null = fopen("/dev/null", "w");
    m = limba_read(data, size, NULL);
    if (!m)
        m = limba_parse((const char *)data, size, NULL);
    if (m && limba_verify(m, NULL) == 0) {
        p = meri_compile(m, &d);
        if (p && null)
            meri_disasm(p, null);
        meri_program_free(p);
    }
    limba_module_free(m);
    return 0;
}

#ifndef MERI_FUZZER
int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        uint8_t *buf;
        long n;

        if (!f) {
            perror(argv[i]);
            return 1;
        }
        if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 ||
            fseek(f, 0, SEEK_SET) || !(buf = malloc((size_t)n + 1))) {
            perror(argv[i]);
            fclose(f);
            return 1;
        }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            perror(argv[i]);
            free(buf);
            fclose(f);
            return 1;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
    }
    return 0;
}
#endif
