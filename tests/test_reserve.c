/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_reserve - a run where the system limits the virtual memory
 * (ulimit -v, as vm.overcommit_memory=2 does for the commit).
 *
 * In a child process with RLIMIT_AS a little above what it uses: with
 * room for the registers, the frames and a smaller area of slots, the run
 * goes on (meri_run retries the slots smaller); with no room even for the
 * registers, it is MERI_NORESERVE, and meri_report says why. Not under the
 * sanitizers of addresses and threads, which need much more virtual
 * memory than any limit here leaves.
 */
#define _GNU_SOURCE
#include "limba/ir.h"
#include "meri/meri.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SKIP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SKIP 1
#endif
#endif

static const char prog[] = "func @main : fn() -> i64 {\n"
                           "b0:\n"
                           "  v0 = iconst i64 42\n"
                           "  ret v0\n"
                           "}\n";

/* the bytes of virtual memory the process has now */
static unsigned long long vm_now(void)
{
    unsigned long long pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");

    if (f) {
        if (fscanf(f, "%llu", &pages) != 1)
            pages = 0;
        fclose(f);
    }
    return pages * (unsigned long long)sysconf(_SC_PAGESIZE);
}

/* in a child: run p with the address space limited to what it has and
   extra bytes more; 0 if it ended as expected (ok: returned 42; else
   MERI_NORESERVE, the registers at least asked, and the report of it) */
static int child(const limba_module *m, const meri_program *p,
                 unsigned long long extra, int ok)
{
    struct rlimit rl;
    meri_result r;
    FILE *out = fopen("/dev/null", "w");
    char rep[256] = "";
    FILE *rs = fmemopen(rep, sizeof(rep) - 1, "w");

    if (!out || !rs)
        return 2;
    rl.rlim_cur = rl.rlim_max = vm_now() + extra;
    if (setrlimit(RLIMIT_AS, &rl))
        return 3;
    meri_run(
        p, "main",
        &(meri_env){.in = stdin, .out = out, .max_memory = (uint64_t)1 << 30},
        &r);
    if (ok)
        return r.status == MERI_OK && r.ret == 42 ? 0 : 4;
    if (r.status != MERI_NORESERVE || r.code < (int64_t)128 << 20)
        return 5;
    if (meri_report(m, &r, rs) != 1)
        return 8;
    fclose(rs);
    return !strncmp(rep, "test_reserve: cannot reserve ", 29) &&
                   strstr(rep, " for the run: the virtual memory is limited")
               ? 0
               : 9;
}

static int in_child(const limba_module *m, const meri_program *p,
                    unsigned long long extra, int ok)
{
    int st = 0;
    pid_t pid = fork();

    if (pid < 0)
        return 6;
    if (!pid)
        _exit(child(m, p, extra, ok));
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return 7;
    return WEXITSTATUS(st);
}

int main(void)
{
    int failures = 0, e;
    limba_diag d;
    meri_diag md;
    limba_module *m;
    meri_program *p;

    limba_set_program("test_reserve");
#ifdef SKIP
    puts("test_reserve: skipped under this sanitizer");
    return 0;
#endif
    m = limba_parse(prog, sizeof(prog) - 1, &d);
    if (!m || limba_verify(m, &d)) {
        fprintf(stderr, "test_reserve: IR refused: %s\n", d.msg);
        return 1;
    }
    p = meri_compile(m, NULL, &md);
    if (!p) {
        fprintf(stderr, "test_reserve: not compiled: %s\n", md.msg);
        limba_module_free(m);
        return 1;
    }
    /* 400 MiB: the registers (128 MiB) and frames (48 MiB) fit, the
       slots only after the budget of 1 GiB is halved twice */
    e = in_child(m, p, (unsigned long long)400 << 20, 1);
    if (e) {
        fprintf(stderr, "test_reserve: 400 MiB more: failure %d\n", e);
        failures++;
    }
    /* 64 MiB: not even the registers */
    e = in_child(m, p, (unsigned long long)64 << 20, 0);
    if (e) {
        fprintf(stderr, "test_reserve: 64 MiB more: failure %d\n", e);
        failures++;
    }
    meri_program_free(p);
    limba_module_free(m);
    printf("test_reserve: 2 cases, %d failures\n", failures);
    return failures != 0;
}
