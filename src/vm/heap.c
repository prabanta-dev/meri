/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.c - the memory of mem_alloc (heap.h): small blocks in arenas of one
 * size class, large ones from malloc with a map of the addresses ever
 * given out (open addressing, never shrinking: malloc reuses addresses,
 * so it holds about as many entries as the peak of large blocks alive).
 */
#include "vm/heap.h"

#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define SLABS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLABS 0
#endif
#endif
#ifndef SLABS
#define SLABS 1
#endif

#define ARENA ((uintptr_t)1 << 20)
#define ARENA_HEAD 64 /* the class of the arena, then its blocks */
#define HEAD 16       /* before every small block */

/* the header of a small block */
typedef struct {
    uint16_t gen;
    uint8_t live, retired;
    uint32_t pad[3];
} slot_head;

static size_t hash(uintptr_t p, size_t cap)
{
    return (size_t)(((p >> 3) * 0x9e3779b97f4a7c15ull) >> 24) & (cap - 1);
}

/* the size class of n bytes, 0 .. MERI_SLAB_CLASSES - 1 */
static unsigned klass(uint64_t n)
{
    return n ? (unsigned)((n - 1) / 16) : 0;
}

uint64_t meri_heap_charge(uint64_t n)
{
    if (SLABS && n <= MERI_SLAB_MAX)
        return 16 * ((uint64_t)klass(n) + 1);
    return n ? n : 1;
}

/* ---- the large blocks ---- */

static meri_heap_entry *find(const meri_heap *h, uintptr_t p)
{
    size_t k;

    if (!h->cap)
        return NULL;
    for (k = hash(p, h->cap); h->e[k].addr; k = (k + 1) & (h->cap - 1))
        if (h->e[k].addr == p)
            return &h->e[k];
    return NULL;
}

static bool grow(meri_heap *h)
{
    size_t cap = h->cap ? 2 * h->cap : 64, i, j;
    meri_heap_entry *e;

    if (2 * (h->used + 1) <= h->cap)
        return true;
    e = calloc(cap, sizeof(*e));
    if (!e)
        return false;
    for (i = 0; i < h->cap; i++) {
        if (!h->e[i].addr)
            continue;
        for (j = hash(h->e[i].addr, cap); e[j].addr; j = (j + 1) & (cap - 1))
            ;
        e[j] = h->e[i];
    }
    free(h->e);
    h->e = e;
    h->cap = cap;
    return true;
}

static bool retire(meri_heap *h, void *p)
{
    if (h->nretired == h->capretired) {
        size_t cap = h->capretired ? 2 * h->capretired : 16;
        void **r = realloc(h->retired, cap * sizeof(*r));
        if (!r)
            return false;
        h->retired = r;
        h->capretired = cap;
    }
    h->retired[h->nretired++] = p;
    return true;
}

static uint64_t large_alloc(meri_heap *h, uint64_t n)
{
    for (;;) {
        meri_heap_entry *e;
        void *p;
        uintptr_t a;
        size_t k;

        if (n > PTRDIFF_MAX || !grow(h))
            return 0;
        p = calloc(n ? (size_t)n : 1, 1);
        if (!p)
            return 0;
        a = (uintptr_t)p;
        if (a >> MERI_ADDR_BITS) { /* no room for a tag: refused */
            free(p);
            return 0;
        }
        e = find(h, a);
        if (!e) {
            for (k = hash(a, h->cap); h->e[k].addr; k = (k + 1) & (h->cap - 1))
                ;
            e = &h->e[k];
            *e = (meri_heap_entry){a, 0, 0, 0};
            h->used++;
        }
        if (h->tagged && e->gen == MERI_GEN_LAST - 1) {
            /* the last generation: this address is never given out
               again, the block stays allocated */
            e->gen = MERI_GEN_LAST;
            if (!retire(h, p)) {
                free(p);
                return 0;
            }
            continue;
        }
        if (h->tagged)
            e->gen++;
        e->size = n ? n : 1;
        e->live = 1;
        return (uint64_t)a | (uint64_t)e->gen << MERI_ADDR_BITS;
    }
}

/* ---- the small blocks ---- */

static bool is_arena(const meri_heap *h, uintptr_t base)
{
    size_t k;

    if (!h->capa)
        return false;
    for (k = hash(base, h->capa); h->arena[k]; k = (k + 1) & (h->capa - 1))
        if (h->arena[k] == base)
            return true;
    return false;
}

static bool add_arena(meri_heap *h, uintptr_t base)
{
    size_t k;

    if (2 * (h->useda + 1) > h->capa) {
        size_t cap = h->capa ? 2 * h->capa : 16, i, j;
        uintptr_t *a = calloc(cap, sizeof(*a));
        if (!a)
            return false;
        for (i = 0; i < h->capa; i++) {
            if (!h->arena[i])
                continue;
            for (j = hash(h->arena[i], cap); a[j]; j = (j + 1) & (cap - 1))
                ;
            a[j] = h->arena[i];
        }
        free(h->arena);
        h->arena = a;
        h->capa = cap;
    }
    for (k = hash(base, h->capa); h->arena[k]; k = (k + 1) & (h->capa - 1))
        ;
    h->arena[k] = base;
    h->useda++;
    return true;
}

/* a new arena for class c, its blocks ready to be cut; false when memory
   is exhausted */
static bool new_arena(meri_heap *h, unsigned c)
{
    void *p;
    uintptr_t base;

    if (posix_memalign(&p, ARENA, ARENA))
        return false;
    base = (uintptr_t)p;
    if (base >> MERI_ADDR_BITS || !add_arena(h, base)) {
        free(p);
        return false;
    }
    *(uint32_t *)p = c;
    h->bump[c] = (uint8_t *)p + ARENA_HEAD;
    h->bump_end[c] = (uint8_t *)p + ARENA;
    return true;
}

/* the step of the blocks of class c in an arena, and its reciprocal:
   for off < ARENA, (off * STEP_M(c)) >> 40 is off / STEP(c) exactly (the
   error of the rounding, below 2^-20, never reaches the next multiple of
   1 / STEP(c)): a check of every access without a division */
#define STEP(c) (HEAD + 16 * ((uint64_t)(c) + 1))
#define STEP_M(c) ((((uint64_t)1 << 40) + STEP(c) - 1) / STEP(c))
_Static_assert(MERI_SLAB_CLASSES == 16, "step_m has one entry a class");
static const uint64_t step_m[MERI_SLAB_CLASSES] = {
    STEP_M(0),  STEP_M(1),  STEP_M(2),  STEP_M(3), STEP_M(4),  STEP_M(5),
    STEP_M(6),  STEP_M(7),  STEP_M(8),  STEP_M(9), STEP_M(10), STEP_M(11),
    STEP_M(12), STEP_M(13), STEP_M(14), STEP_M(15)};

/* the header of the small block whose bytes begin at a, or NULL */
static slot_head *small_head(const meri_heap *h, uintptr_t a)
{
    uintptr_t base = a & ~(ARENA - 1);
    uint64_t step, off;
    unsigned c;

    if (!SLABS || !is_arena(h, base))
        return NULL;
    c = *(const uint32_t *)base;
    step = STEP(c);
    if (a < base + ARENA_HEAD + HEAD)
        return NULL;
    off = a - (base + ARENA_HEAD + HEAD);
    if (((off * step_m[c]) >> 40) * step != off ||
        a + 16 * ((uint64_t)c + 1) > base + ARENA)
        return NULL;
    return (slot_head *)(a - HEAD);
}

static uint64_t small_alloc(meri_heap *h, uint64_t n)
{
    unsigned c = klass(n);
    uint64_t size = 16 * ((uint64_t)c + 1);

    for (;;) {
        uint8_t *b;
        slot_head *s;

        if (h->freel[c]) {
            b = h->freel[c];
            memcpy(&h->freel[c], b, sizeof(void *));
        } else {
            if (!h->bump[c] || h->bump[c] + HEAD + size > h->bump_end[c]) {
                if (!new_arena(h, c))
                    return 0;
            }
            b = h->bump[c] + HEAD;
            h->bump[c] += HEAD + size;
            memset(b - HEAD, 0, HEAD);
        }
        s = (slot_head *)(b - HEAD);
        if (h->tagged && s->gen == MERI_GEN_LAST - 1) {
            s->gen = MERI_GEN_LAST; /* never given out again */
            s->retired = 1;
            continue;
        }
        if (h->tagged)
            s->gen++;
        s->live = 1;
        memset(b, 0, size);
        return (uint64_t)(uintptr_t)b | (uint64_t)s->gen << MERI_ADDR_BITS;
    }
}

/* the header of the small block alive that v points to, or NULL */
static slot_head *small_alive(const meri_heap *h, uint64_t v)
{
    slot_head *s = small_head(h, (uintptr_t)MERI_ADDR(v));

    if (!s || !s->live ||
        (h->tagged ? s->gen != (uint16_t)(v >> MERI_ADDR_BITS)
                   : v >> MERI_ADDR_BITS != 0))
        return NULL;
    return s;
}

/* ---- both ---- */

uint64_t meri_heap_alloc(meri_heap *h, uint64_t n)
{
    if ((int64_t)n < 0)
        return 0;
    return SLABS && n <= MERI_SLAB_MAX ? small_alloc(h, n) : large_alloc(h, n);
}

static meri_heap_entry *large_alive(const meri_heap *h, uint64_t v)
{
    meri_heap_entry *e = find(h, (uintptr_t)MERI_ADDR(v));

    if (!e || !e->live ||
        (h->tagged ? e->gen != (uint16_t)(v >> MERI_ADDR_BITS)
                   : v >> MERI_ADDR_BITS != 0))
        return NULL;
    return e;
}

bool meri_heap_free(meri_heap *h, uint64_t p, uint64_t *size)
{
    slot_head *s;
    meri_heap_entry *e;

    *size = 0;
    if (!p)
        return true;
    s = small_alive(h, p);
    if (s) {
        uintptr_t a = (uintptr_t)MERI_ADDR(p);
        unsigned c = *(const uint32_t *)(a & ~(ARENA - 1));
        s->live = 0;
        *size = 16 * ((uint64_t)c + 1);
        if (!(h->tagged && s->gen == MERI_GEN_LAST - 1)) {
            memcpy((void *)a, &h->freel[c], sizeof(void *));
            h->freel[c] = (void *)a;
        } else {
            s->gen = MERI_GEN_LAST; /* never given out again */
            s->retired = 1;
        }
        return true;
    }
    e = large_alive(h, p);
    if (!e)
        return false;
    e->live = 0;
    *size = e->size;
    free((void *)e->addr);
    return true;
}

bool meri_heap_live(const meri_heap *h, uint64_t p)
{
    return p && (small_alive(h, p) || large_alive(h, p));
}

void meri_heap_clear(meri_heap *h)
{
    size_t i;

    for (i = 0; i < h->cap; i++)
        if (h->e[i].addr && h->e[i].live)
            free((void *)h->e[i].addr);
    for (i = 0; i < h->nretired; i++)
        free(h->retired[i]);
    for (i = 0; i < h->capa; i++)
        if (h->arena[i])
            free((void *)h->arena[i]);
    free(h->e);
    free(h->retired);
    free(h->arena);
    *h = (meri_heap){0};
}
