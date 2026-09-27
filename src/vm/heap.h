/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.h - the memory of mem_alloc (progetto_ir.md § 4, STRICT): the blocks
 * alive are in a map, so that ptr_live answers and a second mem_free is
 * INVALID_FREE. First cut: a freed block is never given back, so a
 * dangling pointer never equals a new one (as in lir_run).
 */
#ifndef MERI_HEAP_H
#define MERI_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t *key; /* 0 empty, 1 removed */
    size_t cap, used, live;
    void **dead; /* the freed blocks, released at the end */
    size_t ndead, capdead;
} meri_heap;

/* the largest block mem_alloc gives, as lir_run: 1 GiB */
#define MERI_HEAP_MAX ((uint64_t)1 << 30)

/* a zeroed block of n bytes (1 for 0); 0 when n is negative, too large,
   or memory is exhausted: the trap NOMEM */
uint64_t meri_heap_alloc(meri_heap *h, uint64_t n);
/* false if p (not 0) is not a block alive: the trap INVALID_FREE */
bool meri_heap_free(meri_heap *h, uint64_t p);
bool meri_heap_live(const meri_heap *h, uint64_t p);
/* free everything, alive or not */
void meri_heap_clear(meri_heap *h);

#endif
