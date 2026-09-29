/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * abi.c - the System V AMD64 calling convention (abi.h), after "System V
 * Application Binary Interface, AMD64 Architecture Processor Supplement",
 * § 3.2.3: the classification here, the trampoline at the end.
 *
 * A value is split in eightbytes; each gets a class, INTEGER (the
 * registers rdi rsi rdx rcx r8 r9, then rax rdx for a result) or SSE
 * (xmm0..xmm7, then xmm0 xmm1), or the whole value is MEMORY (the stack,
 * or for a result a buffer whose address the caller passes first). A
 * struct of more than 16 bytes, or with a field off its alignment, is
 * MEMORY. An eightbyte is INTEGER if any integer field lies in it, else
 * SSE. A struct passed in registers takes all of them or none: when they
 * are not enough it goes whole to the stack, and the registers stay for
 * the arguments after it. An integer narrower than 64 bits is extended by
 * its sign or with zeros in its register or stack word, as the compilers
 * expect.
 */
#include "vm/abi.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SCALAR(name, k, n)                                                     \
    const meri_abi_type meri_abi_##name = {k, n, n, 0, NULL, NULL}
SCALAR(s8, MERI_ABI_S8, 1);
SCALAR(u8, MERI_ABI_U8, 1);
SCALAR(s16, MERI_ABI_S16, 2);
SCALAR(u16, MERI_ABI_U16, 2);
SCALAR(s32, MERI_ABI_S32, 4);
SCALAR(u32, MERI_ABI_U32, 4);
SCALAR(s64, MERI_ABI_S64, 8);
SCALAR(u64, MERI_ABI_U64, 8);
SCALAR(f32, MERI_ABI_F32, 4);
SCALAR(f64, MERI_ABI_F64, 8);
SCALAR(ptr, MERI_ABI_PTR, 8);
const meri_abi_type meri_abi_void = {MERI_ABI_VOID, 0, 1, 0, NULL, NULL};

#if defined(__x86_64__) && defined(__ELF__) && !defined(_WIN32)
#define SYSV 1
#endif

bool meri_abi_available(const char **why)
{
#ifdef SYSV
    (void)why;
    return true;
#else
    *why = "no calling convention for this machine (System V AMD64 only)";
    return false;
#endif
}

#ifdef SYSV

/* what the trampoline loads and brings back: offsets fixed, see the end */
typedef struct {
    uint64_t gpr[6];
    uint64_t sse[8];
    const uint64_t *stack;
    uint64_t nstack;
    void *fn;
    uint64_t nsse; /* in al: the SSE registers used, for a variadic callee */
    uint64_t rax, rdx, xmm0, xmm1;
} frame;

_Static_assert(offsetof(frame, gpr) == 0 && offsetof(frame, sse) == 48 &&
                   offsetof(frame, stack) == 112 &&
                   offsetof(frame, nstack) == 120 &&
                   offsetof(frame, fn) == 128 && offsetof(frame, nsse) == 136 &&
                   offsetof(frame, rax) == 144 && offsetof(frame, rdx) == 152 &&
                   offsetof(frame, xmm0) == 160 && offsetof(frame, xmm1) == 168,
               "the offsets the trampoline reads");

void meri_abi_trampoline(frame *f);

enum { NONE, INTEGER, SSE, MEMORY };

static bool is_float(meri_abi_kind k)
{
    return k == MERI_ABI_F32 || k == MERI_ABI_F64;
}

/* type t is one the ABI carries: a scalar, or a struct of size > 0 and a
   multiple of its alignment whose fields lie at a multiple of their own
   alignment, inside it, and are carried themselves */
static bool valid(const meri_abi_type *t)
{
    uint32_t i;

    if (t->kind == MERI_ABI_VOID || !t->size || !t->align)
        return false;
    if (t->kind != MERI_ABI_STRUCT)
        return true;
    if (!t->nfields || t->size % t->align)
        return false;
    for (i = 0; i < t->nfields; i++) {
        const meri_abi_type *ft = t->fields[i];
        if (!valid(ft) || t->offsets[i] % ft->align ||
            t->offsets[i] > t->size || ft->size > t->size - t->offsets[i])
            return false;
    }
    return true;
}

/* the classes of the eightbytes of a valid type t of at most 16 bytes, at
   offset at of the value */
static void classify_at(const meri_abi_type *t, uint32_t at, int cls[2])
{
    uint32_t i;

    if (t->kind == MERI_ABI_STRUCT) {
        for (i = 0; i < t->nfields; i++)
            classify_at(t->fields[i], at + t->offsets[i], cls);
        return;
    }
    {
        int c = is_float(t->kind) ? SSE : INTEGER, *e = &cls[at / 8];
        if (*e == NONE || (*e == SSE && c == INTEGER))
            *e = c;
    }
}

/* the classes of a value of type t, its eightbytes in *n: MEMORY in
   cls[0] for the stack; false if the ABI does not carry it, or if an
   eightbyte of it is only padding (its class is not certain) */
static bool classify(const meri_abi_type *t, int cls[2], unsigned *n)
{
    unsigned k;

    cls[0] = cls[1] = NONE;
    if (!valid(t))
        return false;
    *n = (t->size + 7) / 8;
    if (t->size > 16) {
        cls[0] = MEMORY;
        return true;
    }
    classify_at(t, 0, cls);
    for (k = 0; k < *n; k++)
        if (cls[k] == NONE)
            return false;
    return true;
}

/* the 64-bit word of an integer scalar at p, extended as its kind says */
static uint64_t widen(meri_abi_kind k, const void *p)
{
    switch (k) {
    case MERI_ABI_S8: {
        int8_t v;
        memcpy(&v, p, 1);
        return (uint64_t)(int64_t)v;
    }
    case MERI_ABI_U8: {
        uint8_t v;
        memcpy(&v, p, 1);
        return v;
    }
    case MERI_ABI_S16: {
        int16_t v;
        memcpy(&v, p, 2);
        return (uint64_t)(int64_t)v;
    }
    case MERI_ABI_U16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    case MERI_ABI_S32: {
        int32_t v;
        memcpy(&v, p, 4);
        return (uint64_t)(int64_t)v;
    }
    case MERI_ABI_U32:
    case MERI_ABI_F32: {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    default: {
        uint64_t v;
        memcpy(&v, p, 8);
        return v;
    }
    }
}

/* eightbyte i of the value of type t at p (the bytes past its size 0) */
static uint64_t eightbyte(const meri_abi_type *t, const void *p, unsigned i)
{
    uint64_t v = 0;
    uint32_t n;

    if (t->kind != MERI_ABI_STRUCT)
        return widen(t->kind, p);
    n = t->size - 8 * i < 8 ? t->size - 8 * i : 8;
    memcpy(&v, (const char *)p + 8 * i, n);
    return v;
}

bool meri_abi_call(void *fn, const meri_abi_type *result, uint32_t n,
                   const meri_abi_type *const *args, const void *const *vals,
                   void *ret, const char **why)
{
    frame f;
    unsigned gi = 0, si = 0, rn = 0, i, k;
    int rcls[2] = {NONE, NONE};
    uint64_t small[64], *stack = small;
    size_t nstack = 0, cap = 64;
    bool ok = false;

    memset(&f, 0, sizeof(f));
    if (n > MERI_ABI_MAX_ARGS) {
        *why = "too many arguments";
        return false;
    }
    if (result->kind != MERI_ABI_VOID) {
        if (!classify(result, rcls, &rn)) {
            *why = "a result the calling convention does not carry";
            return false;
        }
        if (rcls[0] == MEMORY) /* the address of the result, first */
            f.gpr[gi++] = (uint64_t)(uintptr_t)ret;
    }
    for (i = 0; i < n; i++) {
        const meri_abi_type *t = args[i];
        int cls[2];
        unsigned words, need_g = 0, need_s = 0;
        if (t->kind == MERI_ABI_VOID || !classify(t, cls, &words)) {
            *why = "an argument the calling convention does not carry";
            goto done;
        }
        if (cls[0] != MEMORY)
            for (k = 0; k < words; k++)
                cls[k] == SSE ? need_s++ : need_g++;
        if (cls[0] != MEMORY && gi + need_g <= 6 && si + need_s <= 8) {
            for (k = 0; k < words; k++) {
                uint64_t w = eightbyte(t, vals[i], k);
                if (cls[k] == SSE)
                    f.sse[si++] = w;
                else
                    f.gpr[gi++] = w;
            }
            continue;
        }
        /* the stack: the value whole, in words (the registers stay for
           the arguments after it) */
        if (nstack + words > cap) {
            size_t nc = 2 * (nstack + words);
            uint64_t *ns = malloc(nc * sizeof(*ns));
            if (!ns) {
                *why = "out of memory";
                goto done;
            }
            memcpy(ns, stack, nstack * sizeof(*ns));
            if (stack != small)
                free(stack);
            stack = ns;
            cap = nc;
        }
        for (k = 0; k < words; k++)
            stack[nstack++] = eightbyte(t, vals[i], k);
    }
    f.stack = stack;
    f.nstack = nstack;
    f.fn = fn;
    f.nsse = si;
    meri_abi_trampoline(&f);
    if (result->kind != MERI_ABI_VOID && rcls[0] != MEMORY) {
        /* the registers of the result, eightbyte by eightbyte */
        uint64_t w[2];
        unsigned g = 0, s = 0;
        for (k = 0; k < rn; k++)
            w[k] = rcls[k] == SSE ? (s++ ? f.xmm1 : f.xmm0)
                                  : (g++ ? f.rdx : f.rax);
        memcpy(ret, w, result->size);
    }
    ok = true;
done:
    if (stack != small)
        free(stack);
    return ok;
}

/* the trampoline: rdi = the frame. Stack words copied below the return
   address, 16-byte aligned at the call; the registers loaded; the callee
   called; its result registers stored back */
__asm__(".text\n"
        ".globl meri_abi_trampoline\n"
        ".type meri_abi_trampoline, @function\n"
        "meri_abi_trampoline:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    movq %rdi, %rbx\n"
        "    movq 120(%rbx), %rcx\n"
        "    leaq 15(,%rcx,8), %r11\n"
        "    andq $-16, %r11\n"
        "    subq %r11, %rsp\n"
        "    movq 112(%rbx), %rsi\n"
        "    movq %rsp, %rdi\n"
        "    cld\n"
        "    rep movsq\n"
        "    movq 48(%rbx), %xmm0\n"
        "    movq 56(%rbx), %xmm1\n"
        "    movq 64(%rbx), %xmm2\n"
        "    movq 72(%rbx), %xmm3\n"
        "    movq 80(%rbx), %xmm4\n"
        "    movq 88(%rbx), %xmm5\n"
        "    movq 96(%rbx), %xmm6\n"
        "    movq 104(%rbx), %xmm7\n"
        "    movq 0(%rbx), %rdi\n"
        "    movq 8(%rbx), %rsi\n"
        "    movq 16(%rbx), %rdx\n"
        "    movq 24(%rbx), %rcx\n"
        "    movq 32(%rbx), %r8\n"
        "    movq 40(%rbx), %r9\n"
        "    movq 136(%rbx), %rax\n"
        "    callq *128(%rbx)\n"
        "    movq %rax, 144(%rbx)\n"
        "    movq %rdx, 152(%rbx)\n"
        "    movq %xmm0, 160(%rbx)\n"
        "    movq %xmm1, 168(%rbx)\n"
        "    leaq -16(%rbp), %rsp\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size meri_abi_trampoline, .-meri_abi_trampoline\n");

#else /* no calling convention here */

bool meri_abi_call(void *fn, const meri_abi_type *result, uint32_t n,
                   const meri_abi_type *const *args, const void *const *vals,
                   void *ret, const char **why)
{
    (void)fn, (void)result, (void)n, (void)args, (void)vals, (void)ret;
    return meri_abi_available(why);
}

#endif
