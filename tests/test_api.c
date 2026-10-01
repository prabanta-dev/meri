/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_api - Meri as a library, through meri/meri.h only, fed by the front
 * end of Limba (limba/limba_luxia.h) as prabanta feeds it: a function at a
 * time, each body freed right after it is given. What runs so must print
 * what the same module, compiled whole, prints; end the same way, in the
 * same words; and the calls out of order must fail, not run.
 */
#include "limba/limba_luxia.h"
#include "meri/meri.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, cases;

#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            failures++;                                                        \
            fprintf(stderr, "test_api: line %d: ", __LINE__);                  \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* Fib calls Later, given after it; a record with a String in a slot, its
   reference released when Name returns */
static const char calls[] = "program Api;\n"
                            "\n"
                            "type\n"
                            "  Item = record\n"
                            "    name: String;\n"
                            "    n: Int32;\n"
                            "  end;\n"
                            "\n"
                            "var\n"
                            "  total: Int64;\n"
                            "\n"
                            "function Fib(n: Int32): Int32;\n"
                            "begin\n"
                            "  if n < 2 then\n"
                            "    return n;\n"
                            "  end;\n"
                            "  return Fib(n - 1) + Later(n - 2);\n"
                            "end Fib;\n"
                            "\n"
                            "function Later(n: Int32): Int32;\n"
                            "begin\n"
                            "  return Fib(n);\n"
                            "end Later;\n"
                            "\n"
                            "function Name(i: Int32): String;\n"
                            "begin\n"
                            "  var it: Item;\n"
                            "  it.name := \"item\";\n"
                            "  it.n := i;\n"
                            "  return it.name;\n"
                            "end Name;\n"
                            "\n"
                            "begin\n"
                            "  total := 0;\n"
                            "  for var i: Int32 := 1 to 10 do\n"
                            "    total := total + Int64(Fib(i));\n"
                            "  end;\n"
                            "  writeln(total, \" \", Name(7));\n"
                            "end Api.\n";

/* an index out of its range, in a function given before main */
static const char trap[] = "program Trap;\n"
                           "\n"
                           "var\n"
                           "  a: array[Int32 range 1..3] of Int32;\n"
                           "\n"
                           "function At(i: Int32): Int32;\n"
                           "begin\n"
                           "  return a[i];\n"
                           "end At;\n"
                           "\n"
                           "begin\n"
                           "  writeln(\"before\");\n"
                           "  writeln(At(4));\n"
                           "end Trap.\n";

static const char halts[] = "program Halts;\n"
                            "begin\n"
                            "  writeln(\"bye\");\n"
                            "  halt(3);\n"
                            "end Halts.\n";

/* the consumer: Meri compiles each function as the front end gives it */
typedef struct {
    meri_program *p;
    meri_diag d;
    bool failed, ended;
    limba_id given; /* functions given */
} feed;

static void begin(void *ctx, const limba_module *m)
{
    feed *f = ctx;
    f->p = meri_compile_begin(m, &f->d);
    f->failed = !f->p;
}

static int func(void *ctx, limba_module *m, limba_id fid)
{
    feed *f = ctx;
    (void)m;
    f->given++;
    if (f->failed || !meri_compile_func(f->p, fid, &f->d))
        f->failed = true;
    return f->failed;
}

static void end(void *ctx, const limba_module *m, int status)
{
    feed *f = ctx;
    (void)m;
    if (status == LIMBA_LUXIA_OK && !f->failed)
        f->ended = meri_compile_end(f->p, &f->d);
}

/* what one run did */
typedef struct {
    meri_result r;
    int status; /* of meri_report */
    char out[256], err[256];
} outcome;

/* p run, its output and its report into o */
static void run(const limba_module *m, const meri_program *p, outcome *o)
{
    char *ob = NULL, *eb = NULL;
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&ob, &on), *err = open_memstream(&eb, &en);
    FILE *in = fopen("/dev/null", "r");
    meri_env env;

    memset(o, 0, sizeof(*o));
    if (!out || !err || !in) {
        CHECK(false, "no stream for the run");
    } else {
        env = (meri_env){0, NULL, in, out, (uint64_t)64 << 20};
        meri_run(p, "main", &env, &o->r);
        meri_flush(out, &o->r);
        o->status = meri_report(m, &o->r, err);
    }
    if (out)
        fclose(out);
    if (err)
        fclose(err);
    if (in)
        fclose(in);
    snprintf(o->out, sizeof(o->out), "%s", ob ? ob : "");
    snprintf(o->err, sizeof(o->err), "%s", eb ? eb : "");
    free(ob);
    free(eb);
}

/* name compiled streamed and whole: the same outcome, and the one
   expected */
static void both(const char *name, const char *text, const char *want_out,
                 int want_status, bool want_err)
{
    limba_luxia_options o = {.level = 1, .verify = true};
    feed f = {0};
    limba_luxia_consumer c = {&f, begin, func, end, NULL, false};
    limba_module *ms = NULL, *mw = NULL;
    meri_program *pw = NULL;
    meri_diag d;
    outcome a, b;
    int r;

    cases++;
    r = limba_luxia_compile_text(name, text, strlen(text), &o, &c, &ms);
    CHECK(r == LIMBA_LUXIA_OK && ms, "%s: the front end gave %d", name, r);
    CHECK(f.ended, "%s: streamed: %s", name, f.d.msg);
    CHECK(!ms || f.given == ms->nfuncs,
          "%s: %" PRIu32 " functions given of %" PRIu32, name, f.given,
          ms->nfuncs);
    if (r == LIMBA_LUXIA_OK && ms && f.ended) {
        run(ms, f.p, &a);
        CHECK(!strcmp(a.out, want_out), "%s: streamed printed \"%s\"", name,
              a.out);
        CHECK(a.status == want_status, "%s: streamed: status %d", name,
              a.status);
        CHECK(!*a.err == !want_err, "%s: streamed reported \"%s\"", name,
              a.err);
        CHECK(a.r.live_strings == 0 || want_status != 0,
              "%s: %" PRIu64 " strings alive", name, a.r.live_strings);

        /* the same module whole, its bodies kept */
        c = (limba_luxia_consumer){.keep_bodies = true};
        r = limba_luxia_compile_text(name, text, strlen(text), &o, &c, &mw);
        pw = r == LIMBA_LUXIA_OK && mw ? meri_compile(mw, &d) : NULL;
        CHECK(pw, "%s: whole: %s", name, pw || !mw ? d.msg : "front end");
        if (pw) {
            run(mw, pw, &b);
            CHECK(!strcmp(a.out, b.out) && !strcmp(a.err, b.err) &&
                      a.status == b.status && a.r.status == b.r.status &&
                      a.r.code == b.r.code && a.r.pos == b.r.pos,
                  "%s: streamed \"%s\" \"%s\" %d, whole \"%s\" \"%s\" %d", name,
                  a.out, a.err, a.status, b.out, b.err, b.status);
        }
    }
    meri_program_free(f.p);
    meri_program_free(pw);
    limba_module_free(ms);
    limba_module_free(mw);
}

/* the calls out of order fail, and nothing runs */
static void misuse(void)
{
    limba_luxia_consumer c = {.keep_bodies = true};
    limba_module *m = NULL;
    meri_program *p;
    meri_diag d;
    outcome o;
    int r;

    cases++;
    r = limba_luxia_compile_text("calls.luxia", calls, strlen(calls), NULL, &c,
                                 &m);
    CHECK(r == LIMBA_LUXIA_OK && m, "misuse: the front end gave %d", r);
    if (!m)
        return;

    /* before the end: nothing runs */
    p = meri_compile_begin(m, &d);
    CHECK(p, "misuse: begin: %s", d.msg);
    if (p) {
        CHECK(meri_compile_func(p, 1, &d), "misuse: func 1: %s", d.msg);
        run(m, p, &o);
        CHECK(o.r.status == MERI_BADENTRY, "misuse: ran before the end: %d",
              o.r.status);
        /* twice, then nothing more */
        CHECK(!meri_compile_func(p, 1, &d) && *d.msg,
              "misuse: function 1 given twice");
        CHECK(!meri_compile_func(p, 2, &d), "misuse: given after a failure");
        CHECK(!meri_compile_end(p, &d), "misuse: ended after a failure");
        meri_program_free(p);
    }

    /* a function never given, its body there: the end fails */
    p = meri_compile_begin(m, &d);
    if (p) {
        CHECK(!meri_compile_end(p, &d) && strstr(d.msg, "never given"),
              "misuse: ended without the bodies: %s", d.msg);
        meri_program_free(p);
    }

    /* not declared */
    p = meri_compile_begin(m, &d);
    if (p) {
        CHECK(!meri_compile_func(p, m->nfuncs, &d),
              "misuse: function %" PRIu32 " given", m->nfuncs);
        meri_program_free(p);
    }
    limba_module_free(m);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    CHECK(meri_version() && *meri_version(), "no version");
    both("calls.luxia", calls, "143 item\n", 0, false);
    both("trap.luxia", trap, "before\n", 1, true);
    both("halts.luxia", halts, "bye\n", 3, false);
    misuse();
    printf("test_api: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
