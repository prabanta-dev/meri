/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.h - the memory of mem_alloc (progetto_ir.md § 4, STRICT).
 *
 * In STRICT a pointer carries the generation of its block in its 16 high
 * bits: every block given out at the same place has a new one, so a
 * dangling pointer is never equal to a pointer to a new object, and
 * ptr_live and mem_free are exact. A place whose generation reaches the
 * last is never given out again. Every access strips the tag (MERI_ADDR).
 * In FB the pointers carry no tag.
 *
 * Small blocks (up to MERI_SLAB_MAX bytes) live in arenas of 1 MiB, one
 * size class each, with a header of 16 bytes before every block (its
 * generation, whether it is alive) and a list of the free ones. Larger
 * blocks come from malloc, with a map of the addresses ever given out.
 * A pointer is checked against the arenas and the map, so no pointer
 * makes the heap read memory it does not own. Under AddressSanitizer
 * every block comes from malloc, so that the sanitizer sees them.
 */
#ifndef MERI_HEAP_H
#define MERI_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the address a pointer holds, without its generation */
#define MERI_ADDR_BITS 48
#define MERI_ADDR(v) ((v) & (((uint64_t)1 << MERI_ADDR_BITS) - 1))
#define MERI_GEN_LAST 0xffffu

#define MERI_SLAB_MAX 256
#define MERI_SLAB_CLASSES (MERI_SLAB_MAX / 16)

typedef struct {
    uintptr_t addr; /* 0: an empty entry */
    uint64_t size;  /* of the block alive there */
    uint16_t gen;   /* the last generation given out at addr */
    uint8_t live;
} meri_heap_entry;

typedef struct {
    bool tagged; /* STRICT: the generation goes into the pointer */
    /* the large blocks */
    meri_heap_entry *e;
    size_t cap, used;
    void **retired; /* blocks at their last generation, freed at the end */
    size_t nretired, capretired;
    /* the small blocks */
    uintptr_t *arena; /* a set of the arenas' bases, open addressing */
    size_t capa, useda;
    void *freel[MERI_SLAB_CLASSES]; /* free blocks, linked in their bytes */
    uint8_t *bump[MERI_SLAB_CLASSES], *bump_end[MERI_SLAB_CLASSES];
} meri_heap;

/* the bytes a block of n takes from the budget */
uint64_t meri_heap_charge(uint64_t n);
/* a zeroed block of n bytes (1 for 0); 0 when n is negative, past the
   address space, or memory is exhausted: the trap NOMEM (the budget of the
   program is counted by the caller, with meri_heap_charge) */
uint64_t meri_heap_alloc(meri_heap *h, uint64_t n);
/* free the block of p (0: nothing) and give its charge in *size; false
   if p is not a block alive: the trap INVALID_FREE */
bool meri_heap_free(meri_heap *h, uint64_t p, uint64_t *size);
bool meri_heap_live(const meri_heap *h, uint64_t p);
/* free everything */
void meri_heap_clear(meri_heap *h);

#endif
