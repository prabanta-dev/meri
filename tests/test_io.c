/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_io - the errors of input and output (progetto_ir.md § 11g), which
 * the cases of test_vm and the random net cannot reach: a run reads a
 * directory, writes to /dev/full or to a pipe nobody reads. A read that
 * fails is the trap IO where it is, never the end of the input; a write
 * that fails is the trap IO without a position; a closed output ends the
 * run with status 141. The output is unbuffered, so that a write fails
 * where it is made.
 */
#include "limba/ir.h"
#include "vm/vm.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures, cases;

#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            failures++;                                                        \
            fprintf(stderr, "test_io: " __VA_ARGS__);                          \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* reads 16 bytes twice, then a line, and prints what it got: "n m b" */
static const char reads[] = "pos 1 \"t.luxia\" 3 5\n"
                            "pos 2 \"t.luxia\" 4 5\n"
                            "func @main : fn() -> i64 {\n"
                            "  slot 16 align 8\n"
                            "  slot 8 align 8\n"
                            "b0:\n"
                            "  v0 = slot ptr $0\n"
                            "  v1 = iconst i64 16\n"
                            "  v2 = call.rt i64 io_read(v0, v1) !1\n"
                            "  v3 = call.rt i64 io_read(v0, v1) !1\n"
                            "  v4 = slot ptr $1\n"
                            "  v5 = sconst str \"\"\n"
                            "  store v5, v4\n"
                            "  v6 = call.rt i1 read_line(v4) !2\n"
                            "  call.rt void print_i64(v2)\n"
                            "  call.rt void print_i64(v3)\n"
                            "  v7 = zext i64 v6\n"
                            "  call.rt void print_i64(v7)\n"
                            "  v8 = iconst i64 0\n"
                            "  ret v8\n"
                            "}\n";

/* a line first, then 16 bytes */
static const char line_first[] = "pos 1 \"t.luxia\" 3 5\n"
                                 "func @main : fn() -> i64 {\n"
                                 "  slot 8 align 8\n"
                                 "b0:\n"
                                 "  v0 = slot ptr $0\n"
                                 "  v1 = sconst str \"\"\n"
                                 "  store v1, v0\n"
                                 "  v2 = call.rt i1 read_line(v0) !1\n"
                                 "  v3 = iconst i64 0\n"
                                 "  ret v3\n"
                                 "}\n";

/* one write, through the routine named by %s */
static const char writes[] = "pos 1 \"t.luxia\" 3 5\n"
                             "func @main : fn() -> i64 {\n"
                             "  slot 8 align 8\n"
                             "b0:\n"
                             "  v0 = slot ptr $0\n"
                             "  v1 = iconst i8 65\n"
                             "  store v1, v0\n"
                             "  v2 = iconst i64 1\n"
                             "  %s\n"
                             "  v3 = iconst i64 0\n"
                             "  ret v3\n"
                             "}\n";

static const char *const write_calls[] = {
    "call.rt void io_write(v0, v2) !1",
    "call.rt void print_i64(v2) !1",
    "call.rt void print_byte(v1) !1",
    "call.rt void print_nl() !1",
    "v4 = iconst i32 233\n  call.rt void print_char(v4) !1",
    "v4 = iconst i32 66\n  call.rt void print_char(v4) !1",
    "v4 = call.rt ref big_from_i64(v2)\n  call.rt void print_big(v4) !1",
};

/* many numbers, through the buffer of the output: a write into the
   buffer may succeed while the flush it caused failed; only the flag of
   the FILE tells, and the run must stop there */
static const char many[] = "pos 1 \"t.luxia\" 3 5\n"
                           "func @main : fn() -> i64 {\n"
                           "b0:\n"
                           "  v0 = iconst i64 0\n"
                           "  v1 = iconst i64 1\n"
                           "  v2 = iconst i64 200000\n"
                           "  br b1(v0)\n"
                           "b1(v3: i64):\n"
                           "  call.rt void print_i64(v3) !1\n"
                           "  v4 = add i64 v3, v1\n"
                           "  v5 = icmp.slt i1 v4, v2\n"
                           "  cbr v5, b1(v4), b2\n"
                           "b2:\n"
                           "  v6 = iconst i64 7\n"
                           "  ret v6\n"
                           "}\n";

/* runs text with in and out; r filled; false if it does not compile */
static bool run(const char *name, const char *text, FILE *in, FILE *out,
                meri_result *r)
{
    limba_diag d = {{0}, 0};
    limba_module *m = limba_parse(text, strlen(text), &d);
    meri_program *p;
    meri_diag md;
    meri_env env;

    cases++;
    if (!m || limba_verify(m, &d) != 0) {
        CHECK(false, "%s: %s", name, d.msg);
        limba_module_free(m);
        return false;
    }
    p = meri_compile(m, &md);
    if (!p) {
        CHECK(false, "%s: %s", name, md.msg);
        limba_module_free(m);
        return false;
    }
    env = (meri_env){0, NULL, in, out, (uint64_t)64 << 20};
    meri_run(p, "main", &env, r);
    meri_program_free(p);
    limba_module_free(m);
    return true;
}

int main(void)
{
    meri_result r;
    FILE *in, *out;
    char buf[64];
    size_t i;
    int fd[2];

    signal(SIGPIPE, SIG_IGN); /* as meri does: EPIPE, not a signal */

    /* a short input: 3 bytes, then the end for good */
    in = fmemopen((void *)"abc", 3, "r");
    out = fmemopen(buf, sizeof(buf), "w");
    if (in && out && run("short input", reads, in, out, &r)) {
        fflush(out);
        CHECK(r.status == MERI_OK, "short input: status %d", r.status);
        CHECK(!strncmp(buf, "300", 3), "short input: printed %.3s", buf);
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);

    /* a directory as the input: the trap IO where it reads, not the end */
    in = fopen("/", "r");
    out = fopen("/dev/null", "w");
    if (in && out && run("unreadable input", reads, in, out, &r)) {
        CHECK(r.status == MERI_TRAP && r.code == LIMBA_TRAP_IO,
              "unreadable input: status %d, code %" PRId64, r.status, r.code);
        CHECK(r.pos == 1, "unreadable input: at position %u", r.pos);
    }
    if (in)
        fclose(in);
    in = fopen("/", "r");
    if (in && out && run("unreadable line", line_first, in, out, &r)) {
        CHECK(r.status == MERI_TRAP && r.code == LIMBA_TRAP_IO,
              "unreadable line: status %d, code %" PRId64, r.status, r.code);
        CHECK(r.pos == 1, "unreadable line: at position %u", r.pos);
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);

    for (i = 0; i < sizeof(write_calls) / sizeof(*write_calls); i++) {
        char text[sizeof(writes) + 64];
        snprintf(text, sizeof(text), writes, write_calls[i]);

        /* a full device: the trap IO, without a position */
        in = fopen("/dev/null", "r");
        out = fopen("/dev/full", "w");
        if (in && out) {
            setvbuf(out, NULL, _IONBF, 0);
            if (run(write_calls[i], text, in, out, &r)) {
                CHECK(r.status == MERI_TRAP && r.code == LIMBA_TRAP_IO,
                      "%s to a full device: status %d, code %" PRId64,
                      write_calls[i], r.status, r.code);
                CHECK(r.pos == 0, "%s to a full device: at position %u",
                      write_calls[i], r.pos);
            }
        }
        if (out)
            fclose(out);

        /* a pipe nobody reads: 141, no trap */
        out = NULL;
        if (pipe(fd) == 0) {
            close(fd[0]);
            out = fdopen(fd[1], "w");
        }
        if (in && out) {
            setvbuf(out, NULL, _IONBF, 0);
            if (run(write_calls[i], text, in, out, &r))
                CHECK(r.status == MERI_HALT && r.code == 141 && r.pos == 0,
                      "%s to a closed pipe: status %d, code %" PRId64 ", at %u",
                      write_calls[i], r.status, r.code, r.pos);
        }
        if (out)
            fclose(out);
        if (in)
            fclose(in);
    }

    /* buffered: to a full device and to a closed pipe, the run stops
       (it would return 7) */
    in = fopen("/dev/null", "r");
    out = fopen("/dev/full", "w");
    if (in && out && run("many to a full device", many, in, out, &r)) {
        CHECK(r.status == MERI_TRAP && r.code == LIMBA_TRAP_IO && r.pos == 0,
              "many to a full device: status %d, code %" PRId64 ", at %u",
              r.status, r.code, r.pos);
    }
    if (out)
        fclose(out);
    out = NULL;
    if (pipe(fd) == 0) {
        close(fd[0]);
        out = fdopen(fd[1], "w");
    }
    if (in && out && run("many to a closed pipe", many, in, out, &r)) {
        CHECK(r.status == MERI_HALT && r.code == 141 && r.pos == 0,
              "many to a closed pipe: status %d, code %" PRId64 ", at %u",
              r.status, r.code, r.pos);
    }
    if (out)
        fclose(out);
    if (in)
        fclose(in);

    printf("test_io: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
