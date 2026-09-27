/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * heap.h - the memory of mem_alloc (progetto_ir.md § 4, STRICT).
 *
 * A block comes from malloc and goes back to it when freed. In STRICT the
 * pointer carries the generation of its block in its 16 high bits: every
 * block at the same address has a new one, so a dangling pointer is never
 * equal to a pointer to a new object, and ptr_live and mem_free are exact.
 * An address whose generation reaches the last is never given out again.
 * A map of the addresses ever used answers, so no pointer makes the heap
 * read memory it does not own. Every access strips the tag (MERI_ADDR).
 * In FB the pointers carry no tag.
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

typedef struct {
    uintptr_t addr; /* 0: an empty entry */
    uint64_t size;  /* of the block alive there */
    uint16_t gen;   /* the last generation given out at addr */
    uint8_t live;
} meri_heap_entry;

typedef struct {
    meri_heap_entry *e;
    size_t cap, used;
    bool tagged;    /* STRICT: the generation goes into the pointer */
    void **retired; /* blocks at their last generation, freed at the end */
    size_t nretired, capretired;
} meri_heap;

/* a zeroed block of n bytes (1 for 0); 0 when n is negative, past the
   address space, or memory is exhausted: the trap NOMEM (the budget of the
   program is counted by the caller) */
uint64_t meri_heap_alloc(meri_heap *h, uint64_t n);
/* free the block of p (0: nothing) and give its size in *size; false if p
   is not a block alive: the trap INVALID_FREE */
bool meri_heap_free(meri_heap *h, uint64_t p, uint64_t *size);
bool meri_heap_live(const meri_heap *h, uint64_t p);
/* free everything */
void meri_heap_clear(meri_heap *h);

#endif
