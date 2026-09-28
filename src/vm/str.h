/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * str.h - the strings of a program: a length, the bytes and a NUL after
 * them (for str_ptr). A str value is a pointer to one, and 0 is "" in
 * memory and in registers alike (progetto_ir.md § 11c).
 *
 * A string counts its references: every register that holds it, every
 * word of memory (store str, retain). The constants of a program and the
 * initial values of its globals are immortal: counts leave them alone.
 */
#ifndef MERI_STR_H
#define MERI_STR_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t rc; /* references; MERI_RC_IMMORTAL for a constant */
    size_t slot; /* where the run lists it, to free what is left */
    size_t len;
    char data[];
} meri_str;

#define MERI_RC_IMMORTAL UINT64_MAX

/* one reference more on the counted value v, a string or a number of
   big.h: both keep their count in their first word (0 and immortals:
   nothing) */
static inline void meri_str_retain(uint64_t v)
{
    uint64_t *rc = (uint64_t *)(uintptr_t)v;

    if (rc && *rc != MERI_RC_IMMORTAL)
        ++*rc;
}

/* a new immortal string of n bytes copied from s (s may be NULL for
   n = 0); NULL when memory is exhausted */
meri_str *meri_str_new(const char *s, size_t n);
/* a new immortal string of n bytes, to be written by the caller */
meri_str *meri_str_alloc(size_t n);

/* the string a value holds: never NULL, 0 is the empty string */
static inline const meri_str *meri_str_of(uint64_t v)
{
    extern const meri_str meri_str_empty;
    return v ? (const meri_str *)(uintptr_t)v : &meri_str_empty;
}

static inline uint64_t meri_str_value(const meri_str *s)
{
    return (uint64_t)(uintptr_t)s;
}

#endif
