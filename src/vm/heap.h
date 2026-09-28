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
    uintptr_t last;   /* the base of the arena found last (arenas are
                         never freed before meri_heap_clear), or 0 */
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
/* free everything */
void meri_heap_clear(meri_heap *h);

/* ---- the small blocks, in line ---- */

#if defined(__SANITIZE_ADDRESS__)
#define MERI_SLABS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MERI_SLABS 0
#endif
#endif
#ifndef MERI_SLABS
#define MERI_SLABS 1
#endif

#define MERI_ARENA ((uintptr_t)1 << 20)
#define MERI_ARENA_HEAD 64 /* the class of the arena, then its blocks */
#define MERI_HEAD 16       /* before every small block */

/* the header of a small block */
typedef struct {
    uint16_t gen;
    uint8_t live, retired;
    uint32_t pad[3];
} meri_slot_head;

/* the step of the blocks of class c in an arena, and its reciprocal:
   for off < MERI_ARENA, (off * MERI_STEP_M(c)) >> 40 is off / MERI_STEP(c)
   exactly (the error of the rounding, below 2^-20, never reaches the next
   multiple of 1 / MERI_STEP(c)): a check of every access without a division */
#define MERI_STEP(c) (MERI_HEAD + 16 * ((uint64_t)(c) + 1))
#define MERI_STEP_M(c) ((((uint64_t)1 << 40) + MERI_STEP(c) - 1) / MERI_STEP(c))
_Static_assert(MERI_SLAB_CLASSES == 16, "step_m has one entry a class");
/* 0, or MERI_STEP(c) - 1 when MERI_STEP(c) is a power of two: a mask then */
#define MERI_STEP_MASK(c)                                                      \
    ((MERI_STEP(c) & (MERI_STEP(c) - 1)) ? 0 : MERI_STEP(c) - 1)
static const uint64_t meri_step_mask[MERI_SLAB_CLASSES] = {
    MERI_STEP_MASK(0),  MERI_STEP_MASK(1),  MERI_STEP_MASK(2),
    MERI_STEP_MASK(3),  MERI_STEP_MASK(4),  MERI_STEP_MASK(5),
    MERI_STEP_MASK(6),  MERI_STEP_MASK(7),  MERI_STEP_MASK(8),
    MERI_STEP_MASK(9),  MERI_STEP_MASK(10), MERI_STEP_MASK(11),
    MERI_STEP_MASK(12), MERI_STEP_MASK(13), MERI_STEP_MASK(14),
    MERI_STEP_MASK(15)};
static const uint64_t meri_step_m[MERI_SLAB_CLASSES] = {
    MERI_STEP_M(0),  MERI_STEP_M(1),  MERI_STEP_M(2),  MERI_STEP_M(3),
    MERI_STEP_M(4),  MERI_STEP_M(5),  MERI_STEP_M(6),  MERI_STEP_M(7),
    MERI_STEP_M(8),  MERI_STEP_M(9),  MERI_STEP_M(10), MERI_STEP_M(11),
    MERI_STEP_M(12), MERI_STEP_M(13), MERI_STEP_M(14), MERI_STEP_M(15)};

bool meri_heap_find_arena(meri_heap *h, uintptr_t base);

/* base is an arena: the one found last, or one of the set */
static inline bool meri_is_arena(meri_heap *h, uintptr_t base)
{
    return (base && base == h->last) || meri_heap_find_arena(h, base);
}

/* the header of the small block whose bytes begin at a, or NULL */
static inline meri_slot_head *meri_small_head(meri_heap *h, uintptr_t a)
{
    uintptr_t base = a & ~(MERI_ARENA - 1);
    uint64_t step, off;
    unsigned c;

    if (!MERI_SLABS || !meri_is_arena(h, base))
        return NULL;
    c = *(const uint32_t *)base;
    step = MERI_STEP(c);
    if (a < base + MERI_ARENA_HEAD + MERI_HEAD)
        return NULL;
    off = a - (base + MERI_ARENA_HEAD + MERI_HEAD);
    if (meri_step_mask[c] ? (off & meri_step_mask[c]) != 0
                          : ((off * meri_step_m[c]) >> 40) * step != off)
        return NULL;
    if (a + 16 * ((uint64_t)c + 1) > base + MERI_ARENA)
        return NULL;
    return (meri_slot_head *)(a - MERI_HEAD);
}

static inline meri_slot_head *meri_small_alive(meri_heap *h, uint64_t v)
{
    meri_slot_head *s = meri_small_head(h, (uintptr_t)MERI_ADDR(v));

    if (!s || !s->live ||
        (h->tagged ? s->gen != (uint16_t)(v >> MERI_ADDR_BITS)
                   : v >> MERI_ADDR_BITS != 0))
        return NULL;
    return s;
}

bool meri_heap_large_live(const meri_heap *h, uint64_t p);

/* ptr_live: p is the pointer of a block alive (the small ones in line:
   the interpreter checks every access) */
static inline bool meri_heap_live(meri_heap *h, uint64_t p)
{
    return p && (meri_small_alive(h, p) || meri_heap_large_live(h, p));
}

#endif
