/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.c - the memory of mem_alloc (heap.h): a map of the blocks alive,
 * open addressing on the address.
 */
#include "vm/heap.h"

#include <stddef.h>
#include <stdlib.h>

static size_t slot_of(uintptr_t p, size_t cap)
{
    return (size_t)(((p >> 3) * 0x9e3779b97f4a7c15ull) >> 24) & (cap - 1);
}

/* the index of p, or cap if absent */
static size_t find(const meri_heap *h, uintptr_t p)
{
    size_t k;

    if (!h->live)
        return h->cap;
    for (k = slot_of(p, h->cap); h->key[k]; k = (k + 1) & (h->cap - 1))
        if (h->key[k] == p)
            return k;
    return h->cap;
}

/* room for one more key; false when memory is exhausted */
static bool grow(meri_heap *h)
{
    size_t cap = h->cap ? h->cap : 64, i, j;
    uintptr_t *key;

    if (2 * (h->used + 1) <= h->cap)
        return true;
    while (4 * (h->live + 1) > cap)
        cap *= 2;
    key = calloc(cap, sizeof(*key));
    if (!key)
        return false;
    for (i = 0; i < h->cap; i++) {
        if (h->key[i] <= 1)
            continue;
        for (j = slot_of(h->key[i], cap); key[j]; j = (j + 1) & (cap - 1))
            ;
        key[j] = h->key[i];
    }
    free(h->key);
    h->key = key;
    h->cap = cap;
    h->used = h->live;
    return true;
}

uint64_t meri_heap_alloc(meri_heap *h, uint64_t n)
{
    void *p;
    size_t k;

    if ((int64_t)n < 0 || n > PTRDIFF_MAX || !grow(h))
        return 0;
    p = calloc(n ? (size_t)n : 1, 1);
    if (!p)
        return 0;
    for (k = slot_of((uintptr_t)p, h->cap); h->key[k] > 1;
         k = (k + 1) & (h->cap - 1))
        ;
    if (!h->key[k])
        h->used++;
    h->key[k] = (uintptr_t)p;
    h->live++;
    return (uint64_t)(uintptr_t)p;
}

bool meri_heap_free(meri_heap *h, uint64_t p)
{
    size_t k;

    if (!p)
        return true;
    k = find(h, (uintptr_t)p);
    if (k == h->cap)
        return false;
    if (h->ndead == h->capdead) {
        size_t cap = h->capdead ? 2 * h->capdead : 64;
        void **dead = realloc(h->dead, cap * sizeof(*dead));
        if (dead) {
            h->dead = dead;
            h->capdead = cap;
        }
    }
    /* without room in the list the block is dead all the same, and only
       its bytes stay until the process ends */
    if (h->ndead < h->capdead)
        h->dead[h->ndead++] = (void *)(uintptr_t)p;
    h->key[k] = 1;
    h->live--;
    return true;
}

bool meri_heap_live(const meri_heap *h, uint64_t p)
{
    return p && find(h, (uintptr_t)p) != h->cap;
}

void meri_heap_clear(meri_heap *h)
{
    size_t i;

    for (i = 0; i < h->cap; i++)
        if (h->key[i] > 1)
            free((void *)h->key[i]);
    for (i = 0; i < h->ndead; i++)
        free(h->dead[i]);
    free(h->key);
    free(h->dead);
    *h = (meri_heap){0};
}
