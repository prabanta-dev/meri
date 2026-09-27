/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.c - the memory of mem_alloc (heap.h): a map of the addresses ever
 * given out, open addressing, never shrinking (malloc reuses addresses,
 * so it holds about as many entries as the peak of blocks alive).
 */
#include "vm/heap.h"

#include <stdlib.h>

static size_t slot_of(uintptr_t p, size_t cap)
{
    return (size_t)(((p >> 3) * 0x9e3779b97f4a7c15ull) >> 24) & (cap - 1);
}

/* the entry of address p, or NULL */
static meri_heap_entry *find(const meri_heap *h, uintptr_t p)
{
    size_t k;

    if (!h->cap)
        return NULL;
    for (k = slot_of(p, h->cap); h->e[k].addr; k = (k + 1) & (h->cap - 1))
        if (h->e[k].addr == p)
            return &h->e[k];
    return NULL;
}

/* room for one more address; false when memory is exhausted */
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
        for (j = slot_of(h->e[i].addr, cap); e[j].addr; j = (j + 1) & (cap - 1))
            ;
        e[j] = h->e[i];
    }
    free(h->e);
    h->e = e;
    h->cap = cap;
    return true;
}

/* keep block p forever: its address is at its last generation */
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

uint64_t meri_heap_alloc(meri_heap *h, uint64_t n)
{
    for (;;) {
        meri_heap_entry *e;
        void *p;
        uintptr_t a;
        size_t k;

        if ((int64_t)n < 0 || n > PTRDIFF_MAX || !grow(h))
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
            for (k = slot_of(a, h->cap); h->e[k].addr;
                 k = (k + 1) & (h->cap - 1))
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

/* the entry of the block alive that pointer v points to, or NULL */
static meri_heap_entry *alive(const meri_heap *h, uint64_t v)
{
    meri_heap_entry *e = find(h, (uintptr_t)MERI_ADDR(v));

    if (!e || !e->live ||
        (h->tagged && e->gen != (uint16_t)(v >> MERI_ADDR_BITS)) ||
        (!h->tagged && v >> MERI_ADDR_BITS))
        return NULL;
    return e;
}

bool meri_heap_free(meri_heap *h, uint64_t p, uint64_t *size)
{
    meri_heap_entry *e;

    *size = 0;
    if (!p)
        return true;
    e = alive(h, p);
    if (!e)
        return false;
    e->live = 0;
    *size = e->size;
    free((void *)e->addr);
    return true;
}

bool meri_heap_live(const meri_heap *h, uint64_t p)
{
    return p && alive(h, p);
}

void meri_heap_clear(meri_heap *h)
{
    size_t i;

    for (i = 0; i < h->cap; i++)
        if (h->e[i].addr && h->e[i].live)
            free((void *)h->e[i].addr);
    for (i = 0; i < h->nretired; i++)
        free(h->retired[i]);
    free(h->e);
    free(h->retired);
    *h = (meri_heap){0};
}
