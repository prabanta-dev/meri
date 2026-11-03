/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_api - what meri.h promises of the options of a compilation and of
 * the result of a run: the engine chosen when the compilation begins (an
 * engine not in this Meri is an error), the words of an exception owned
 * by the result (alive after the program runs again and after it is
 * freed, freed by meri_result_free any number of times), the workers of
 * meri_env taken whatever their number.
 */
#include "limba/ir.h"
#include "meri/meri.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cases, failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        cases++;                                                               \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "test_api: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* raises 1001 with the words given by main's argument count: "first"
   when it has none, "second" else */
static const char prog[] = "language \"luxia\"\n"
                           "exception 1001 \"BadDigit\"\n"
                           "\n"
                           "func @main : fn() -> i64 {\n"
                           "b0:\n"
                           "  v0 = iconst i32 1001\n"
                           "  v1 = iconst i32 1\n"
                           "  v2 = call.rt i32 arg_count()\n"
                           "  v3 = iconst i32 0\n"
                           "  v4 = icmp.eq i1 v2, v3\n"
                           "  cbr v4, b1, b2\n"
                           "b1:\n"
                           "  v5 = sconst str \"first\"\n"
                           "  raise v0, v1, v5\n"
                           "b2:\n"
                           "  v6 = sconst str \"second\"\n"
                           "  raise v0, v1, v6\n"
                           "}\n";

static bool words(const meri_result *r, const char *w)
{
    return r->status == MERI_TRAP && r->code == 1001 && r->msg &&
           r->msglen == strlen(w) && !memcmp(r->msg, w, r->msglen);
}

int main(void)
{
    limba_diag d = {{0}, 0};
    limba_module *m;
    meri_program *p;
    meri_diag md;
    meri_result r1 = {0}, r2 = {0};
    char *argv[] = {"x"};
    FILE *out = fopen("/dev/null", "w");

    limba_set_program("test_api");
    m = limba_parse(prog, sizeof(prog) - 1, &d);
    if (!out || !m || limba_verify(m, &d)) {
        fprintf(stderr, "test_api: IR refused: %s\n", d.msg);
        limba_module_free(m);
        return 1;
    }

    /* the engines */
    p = meri_compile(m, &(meri_options){.engine = MERI_ENGINE_AOT}, &md);
    CHECK(!p && strstr(md.msg, "aot"), "aot: compiled, or said \"%s\"", md.msg);
    meri_program_free(p);
    p = meri_compile_begin(m, &(meri_options){.engine = 7}, &md);
    CHECK(!p && md.msg[0], "engine 7: began, or said nothing");
    meri_program_free(p);
    p = meri_compile(m, &(meri_options){.engine = MERI_ENGINE_VM}, &md);
    CHECK(p, "vm: not compiled: %s", md.msg);
    meri_program_free(p);
    p = meri_compile(m, NULL, &md);
    CHECK(p, "no options: not compiled: %s", md.msg);
    if (!p) {
        limba_module_free(m);
        return 1;
    }

    /* the words are the result's: a second run does not change them */
    meri_run(p, "main",
             &(meri_env){.in = stdin, .out = out, .max_memory = 64 << 20}, &r1);
    meri_run(p, "main",
             &(meri_env){.argc = 1,
                         .argv = argv,
                         .in = stdin,
                         .out = out,
                         .max_memory = 64 << 20,
                         .threads = 1000},
             &r2);
    CHECK(words(&r1, "first"), "first run: status %d, code %" PRId64, r1.status,
          r1.code);
    CHECK(words(&r2, "second"), "second run: status %d, code %" PRId64,
          r2.status, r2.code);
    meri_program_free(p);
    CHECK(words(&r1, "first"), "first run: words lost with the program");
    meri_result_free(&r1);
    CHECK(!r1.msg && !r1.msglen && r1.status == MERI_OK,
          "freed: the result not zeroed");
    meri_result_free(&r1); /* again: nothing */
    meri_result_free(&r2);

    limba_module_free(m);
    fclose(out);
    printf("test_api: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
